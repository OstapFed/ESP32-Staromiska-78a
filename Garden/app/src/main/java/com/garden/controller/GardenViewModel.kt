package com.garden.controller

import android.app.Application
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import com.garden.controller.data.BrokerSettings
import com.garden.controller.data.GardenStatus
import com.garden.controller.data.HistoryPoint
import com.garden.controller.data.HistoryStore
import com.garden.controller.data.SettingsStore
import com.garden.controller.mqtt.MqttClient
import java.util.Locale
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt

enum class Link { IDLE, CONNECTING, CONNECTED, FAILED }

class GardenViewModel(app: Application) : AndroidViewModel(app) {
    private val settingsStore = SettingsStore(app)
    private val historyStore = HistoryStore(app)
    private val main = Handler(Looper.getMainLooper())

    var link by mutableStateOf(Link.IDLE); private set
    var linkError by mutableStateOf<String?>(null); private set
    var status by mutableStateOf<GardenStatus?>(null); private set
    var controllerOnline by mutableStateOf(false); private set
    var lastUpdate by mutableStateOf(0L); private set
    var nowMs by mutableStateOf(System.currentTimeMillis()); private set
    var targetSetpoint by mutableStateOf<Double?>(null); private set
    var toast by mutableStateOf<String?>(null); private set
    var settings by mutableStateOf(settingsStore.load()); private set
    val history = mutableStateListOf<HistoryPoint>()

    private var wantConnected = false
    private var attempt = 0
    private var session = 0
    private var lastHistoryTime = 0L
    private var lastHistorySave = 0L
    private var setpointRunnable: Runnable? = null

    private val client = MqttClient(
        onConnected = { main.post { handleConnected() } },
        onDisconnected = { reason -> main.post { handleDisconnected(reason) } },
        onMessage = { topic, data -> main.post { handleMessage(topic, data) } }
    )

    private val ticker = object : Runnable {
        override fun run() {
            nowMs = System.currentTimeMillis()
            main.postDelayed(this, 10_000)
        }
    }

    init {
        history.addAll(historyStore.load())
        lastHistoryTime = history.lastOrNull()?.time ?: 0L
        main.postDelayed(ticker, 10_000)
    }

    // ---------------- Підключення ----------------

    fun connect() {
        if (!settings.isValid) {
            link = Link.FAILED
            linkError = "Заповніть налаштування брокера"
            return
        }
        wantConnected = true
        session++
        link = Link.CONNECTING
        linkError = null
        val base = settings.base
        client.connect(
            MqttClient.Config(
                host = settings.host.trim(),
                port = settings.port,
                tls = settings.tls,
                username = settings.username,
                password = settings.password,
                clientId = settingsStore.clientId(),
                subscriptions = listOf("$base/state", "$base/availability")
            )
        )
    }

    fun reconnect() {
        client.disconnect()
        connect()
    }

    fun apply(newSettings: BrokerSettings) {
        val cleanBase = newSettings.base.trim().trim('/').ifEmpty { "garden/controller" }
        val s = newSettings.copy(host = newSettings.host.trim(), base = cleanBase)
        settingsStore.save(s)
        settings = s
        status = null
        controllerOnline = false
        reconnect()
    }

    /** Додаток на екрані. */
    fun resume() {
        if (settings.isValid && link != Link.CONNECTED && link != Link.CONNECTING) connect()
    }

    /** Додаток у фоні: Android усе одно обмежує мережу, тому закриваємо чисто. */
    fun pause() {
        saveHistory()
        wantConnected = false
        session++
        client.disconnect()
        link = Link.IDLE
    }

    private fun handleConnected() {
        attempt = 0
        link = Link.CONNECTED
        linkError = null
    }

    private fun handleDisconnected(reason: String?) {
        link = Link.FAILED
        linkError = reason ?: "З'єднання втрачено"
        if (!wantConnected) return
        attempt++
        val delay = min(30.0, 2.0.pow(min(attempt, 5).toDouble())).toLong() * 1000L
        val token = session
        main.postDelayed({
            if (wantConnected && session == token) connect()
        }, delay)
    }

    // ---------------- Вхідні повідомлення ----------------

    private fun handleMessage(topic: String, data: ByteArray) {
        val base = settings.base
        if (topic == "$base/availability") {
            controllerOnline = String(data, Charsets.UTF_8) == "online"
            return
        }
        if (topic != "$base/state") return
        val s = GardenStatus.parse(String(data, Charsets.UTF_8)) ?: return
        status = s
        lastUpdate = System.currentTimeMillis()
        controllerOnline = true

        val target = targetSetpoint
        val actual = s.boilerSetpoint
        if (target != null && actual != null && kotlin.math.abs(target - actual) < 0.01) {
            targetSetpoint = null
        }
        addHistory(s)
    }

    // ---------------- Команди ----------------

    private fun publish(sub: String, payload: String) {
        if (link != Link.CONNECTED || !client.publish("${settings.base}/$sub", payload)) {
            showToast("Немає зв'язку з брокером")
        }
    }

    fun setLight(index: Int, on: Boolean) = publish("light/${index + 1}/set", if (on) "ON" else "OFF")
    fun startZone(zone: Int) = publish("water/$zone/set", "START")
    fun stopWatering() = publish("water/stop/set", "STOP")
    fun setMode(mode: Int) = publish("boiler/mode/set", mode.toString())
    fun setVacation(on: Boolean) = publish("vacation/set", if (on) "ON" else "OFF")
    fun resetToSchedule() = publish("boiler/setpoint/reset", "1")

    /** Крок уставки ±0,5 °C: інтерфейс реагує одразу, команда йде після короткої паузи. */
    fun adjustSetpoint(delta: Double) {
        val current = targetSetpoint ?: status?.boilerSetpoint ?: 21.0
        val next = ((current + delta) * 2.0).roundToInt() / 2.0
        targetSetpoint = next.coerceIn(5.0, 30.0)

        setpointRunnable?.let { main.removeCallbacks(it) }
        val r = Runnable {
            val v = targetSetpoint ?: return@Runnable
            publish("boiler/setpoint/set", String.format(Locale.US, "%.1f", v))
            // Якщо підтвердження не прийшло — повертаємось до реального значення.
            main.postDelayed({ targetSetpoint = null }, 6000)
        }
        setpointRunnable = r
        main.postDelayed(r, 600)
    }

    fun showToast(text: String) {
        toast = text
        main.postDelayed({ if (toast == text) toast = null }, 2500)
    }

    // ---------------- Історія ----------------

    private fun addHistory(s: GardenStatus) {
        val now = System.currentTimeMillis()
        val t = s.temperature ?: return
        if (now - lastHistoryTime < 60_000) return
        lastHistoryTime = now
        history.add(HistoryPoint(now, t, s.boilerSetpoint, s.boilerOn))
        val cutoff = now - 24L * 3600_000L
        history.removeAll { it.time < cutoff }
        if (now - lastHistorySave > 300_000) saveHistory()
    }

    fun saveHistory() {
        lastHistorySave = System.currentTimeMillis()
        historyStore.save(history.toList())
    }

    fun clearHistory() {
        history.clear()
        saveHistory()
    }

    override fun onCleared() {
        main.removeCallbacksAndMessages(null)
        client.disconnect()
        super.onCleared()
    }
}
