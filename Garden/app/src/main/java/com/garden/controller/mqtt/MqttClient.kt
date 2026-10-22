package com.garden.controller.mqtt

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import kotlin.concurrent.thread

/**
 * Мінімальний MQTT 3.1.1 клієнт (TCP + TLS) без сторонніх бібліотек.
 * Сертифікат сервера й ім'я хоста перевіряються системою.
 * Колбеки викликаються з фонових потоків.
 */
class MqttClient(
    private val onConnected: () -> Unit,
    private val onDisconnected: (String?) -> Unit,
    private val onMessage: (String, ByteArray) -> Unit
) {
    data class Config(
        val host: String,
        val port: Int,
        val tls: Boolean,
        val username: String,
        val password: String,
        val clientId: String,
        val subscriptions: List<String>
    )

    @Volatile private var generation = 0
    @Volatile private var socket: Socket? = null
    @Volatile private var out: OutputStream? = null
    @Volatile private var connected = false
    @Volatile private var lastRx = 0L
    private val writer = Executors.newSingleThreadExecutor()

    val isConnected: Boolean get() = connected

    fun connect(cfg: Config) {
        disconnect()
        val gen = generation
        thread(isDaemon = true, name = "mqtt-$gen") { run(cfg, gen) }
    }

    /** Закриває з'єднання без виклику onDisconnected. Мережеві операції йдуть у фоновому потоці. */
    @Synchronized
    fun disconnect() {
        generation++
        connected = false
        val s = socket
        val o = out
        socket = null
        out = null
        if (s == null && o == null) return
        thread(isDaemon = true, name = "mqtt-close") {
            try {
                if (o != null) {
                    synchronized(o) {
                        o.write(byteArrayOf(0xE0.toByte(), 0))
                        o.flush()
                    }
                }
            } catch (e: Exception) {
            }
            try {
                s?.close()
            } catch (e: Exception) {
            }
        }
    }

    /** Повертає false, якщо немає з'єднання. */
    fun publish(topic: String, payload: String): Boolean {
        if (!connected) return false
        write(packet(0x30, str(topic) + payload.toByteArray(Charsets.UTF_8)), generation)
        return true
    }

    // ---------------------------------------------------------------

    private fun run(cfg: Config, gen: Int) {
        try {
            val plain = Socket()
            plain.tcpNoDelay = true
            plain.keepAlive = true
            plain.connect(InetSocketAddress(cfg.host, cfg.port), 10_000)

            val sock: Socket = if (cfg.tls) {
                val factory = SSLSocketFactory.getDefault() as SSLSocketFactory
                val ssl = factory.createSocket(plain, cfg.host, cfg.port, true) as SSLSocket
                val params = ssl.sslParameters
                params.endpointIdentificationAlgorithm = "HTTPS" // перевірка імені хоста
                ssl.sslParameters = params
                ssl.soTimeout = 10_000
                ssl.startHandshake()
                ssl
            } else {
                plain
            }

            if (gen != generation) {
                sock.close()
                return
            }
            sock.soTimeout = 100_000
            socket = sock
            out = BufferedOutputStream(sock.getOutputStream())
            lastRx = System.currentTimeMillis()
            write(buildConnect(cfg), gen)

            val input = DataInputStream(BufferedInputStream(sock.getInputStream()))
            while (gen == generation) {
                val b0 = input.read()
                if (b0 < 0) throw EOFException("Сервер закрив з'єднання")
                var len = 0
                var mult = 1
                var n = 0
                while (true) {
                    val b = input.read()
                    if (b < 0) throw EOFException("Сервер закрив з'єднання")
                    len += (b and 0x7F) * mult
                    mult *= 128
                    n++
                    if ((b and 0x80) == 0) break
                    if (n >= 4) throw IOException("Некоректний MQTT-пакет")
                }
                val payload = ByteArray(len)
                input.readFully(payload)
                lastRx = System.currentTimeMillis()
                handle(cfg, gen, b0, payload)
            }
        } catch (e: Exception) {
            fail(gen, e.message ?: e.javaClass.simpleName)
        }
    }

    private fun handle(cfg: Config, gen: Int, b0: Int, payload: ByteArray) {
        when (b0 shr 4) {
            2 -> { // CONNACK
                val rc = if (payload.size >= 2) payload[1].toInt() and 0xFF else 255
                if (rc != 0) throw IOException(connackText(rc))
                connected = true
                subscribe(cfg, gen)
                startPing(gen)
                onConnected()
            }
            3 -> { // PUBLISH
                val qos = (b0 shr 1) and 0x03
                if (payload.size < 2) return
                val topicLen = ((payload[0].toInt() and 0xFF) shl 8) or (payload[1].toInt() and 0xFF)
                var i = 2
                if (payload.size < i + topicLen) return
                val topic = String(payload, i, topicLen, Charsets.UTF_8)
                i += topicLen
                var pid = 0
                if (qos > 0) {
                    if (payload.size < i + 2) return
                    pid = ((payload[i].toInt() and 0xFF) shl 8) or (payload[i + 1].toInt() and 0xFF)
                    i += 2
                }
                val body = payload.copyOfRange(i, payload.size)
                if (qos == 1) {
                    write(byteArrayOf(0x40, 0x02, (pid shr 8).toByte(), (pid and 0xFF).toByte()), gen)
                }
                onMessage(topic, body)
            }
            else -> Unit // SUBACK, PINGRESP тощо
        }
    }

    private fun connackText(code: Int): String = when (code) {
        1 -> "Брокер не підтримує MQTT 3.1.1"
        2 -> "Брокер відхилив ідентифікатор клієнта"
        3 -> "Сервер MQTT недоступний"
        4 -> "Невірний логін або пароль"
        5 -> "Немає прав доступу (перевірте ACL користувача)"
        else -> "Помилка підключення (код $code)"
    }

    private fun subscribe(cfg: Config, gen: Int) {
        if (cfg.subscriptions.isEmpty()) return
        val body = ByteArrayOutputStream()
        body.write(0)
        body.write(1) // packet id = 1
        for (t in cfg.subscriptions) {
            body.write(str(t))
            body.write(0) // QoS 0
        }
        write(packet(0x82, body.toByteArray()), gen)
    }

    private fun startPing(gen: Int) {
        thread(isDaemon = true, name = "mqtt-ping") {
            while (gen == generation) {
                try {
                    Thread.sleep(30_000)
                } catch (e: InterruptedException) {
                    return@thread
                }
                if (gen != generation) return@thread
                if (System.currentTimeMillis() - lastRx > 100_000) {
                    fail(gen, "Тайм-аут з'єднання")
                    return@thread
                }
                write(byteArrayOf(0xC0.toByte(), 0), gen)
            }
        }
    }

    private fun buildConnect(cfg: Config): ByteArray {
        val body = ByteArrayOutputStream()
        body.write(str("MQTT"))
        body.write(4) // рівень протоколу 3.1.1
        var flags = 0x02 // clean session
        if (cfg.username.isNotEmpty()) {
            flags = flags or 0x80
            if (cfg.password.isNotEmpty()) flags = flags or 0x40
        }
        body.write(flags)
        body.write(0)
        body.write(60) // keepalive 60 c
        body.write(str(cfg.clientId))
        if (cfg.username.isNotEmpty()) {
            body.write(str(cfg.username))
            if (cfg.password.isNotEmpty()) body.write(str(cfg.password))
        }
        return packet(0x10, body.toByteArray())
    }

    private fun write(bytes: ByteArray, gen: Int) {
        writer.execute {
            if (gen != generation) return@execute
            val o = out ?: return@execute
            try {
                synchronized(o) {
                    o.write(bytes)
                    o.flush()
                }
            } catch (e: Exception) {
                fail(gen, e.message ?: "Помилка запису")
            }
        }
    }

    private fun fail(gen: Int, reason: String?) {
        synchronized(this) {
            if (gen != generation) return
            generation++
        }
        connected = false
        closeSocket()
        onDisconnected(reason)
    }

    private fun closeSocket() {
        try {
            socket?.close()
        } catch (e: Exception) {
        }
        socket = null
        out = null
    }

    private fun str(s: String): ByteArray {
        val b = s.toByteArray(Charsets.UTF_8)
        return byteArrayOf((b.size shr 8).toByte(), (b.size and 0xFF).toByte()) + b
    }

    private fun remainingLength(n: Int): ByteArray {
        var x = n
        val o = ByteArrayOutputStream()
        do {
            var digit = x % 128
            x /= 128
            if (x > 0) digit = digit or 0x80
            o.write(digit)
        } while (x > 0)
        return o.toByteArray()
    }

    private fun packet(first: Int, body: ByteArray): ByteArray =
        byteArrayOf(first.toByte()) + remainingLength(body.size) + body
}
