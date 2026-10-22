package com.garden.controller.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Шифрування пароля брокера ключем Android Keystore (AES-256-GCM). */
object SecureStore {
    private const val ALIAS = "garden_broker_key"

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore")
        ks.load(null)
        val existing = ks.getKey(ALIAS, null) as? SecretKey
        if (existing != null) return existing
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(
                ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    fun encrypt(plain: String): String {
        if (plain.isEmpty()) return ""
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val iv = cipher.iv
        val ct = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
        return Base64.encodeToString(iv + ct, Base64.NO_WRAP)
    }

    fun decrypt(stored: String): String {
        if (stored.isEmpty()) return ""
        return try {
            val raw = Base64.decode(stored, Base64.NO_WRAP)
            val iv = raw.copyOfRange(0, 12)
            val ct = raw.copyOfRange(12, raw.size)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
            String(cipher.doFinal(ct), Charsets.UTF_8)
        } catch (e: Exception) {
            ""
        }
    }
}

class SettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences("garden", Context.MODE_PRIVATE)

    fun load(): BrokerSettings = BrokerSettings(
        host = prefs.getString("host", "") ?: "",
        port = prefs.getInt("port", 8883),
        username = prefs.getString("user", "") ?: "",
        password = SecureStore.decrypt(prefs.getString("pass", "") ?: ""),
        base = prefs.getString("base", "garden/controller") ?: "garden/controller",
        tls = prefs.getBoolean("tls", true)
    )

    fun save(s: BrokerSettings) {
        prefs.edit()
            .putString("host", s.host)
            .putInt("port", s.port)
            .putString("user", s.username)
            .putString("pass", SecureStore.encrypt(s.password))
            .putString("base", s.base)
            .putBoolean("tls", s.tls)
            .apply()
    }

    fun clientId(): String {
        val existing = prefs.getString("clientId", null)
        if (existing != null) return existing
        val id = "garden-android-" + UUID.randomUUID().toString().take(8)
        prefs.edit().putString("clientId", id).apply()
        return id
    }
}

class HistoryStore(context: Context) {
    private val file = File(context.filesDir, "history.json")

    fun load(): List<HistoryPoint> {
        return try {
            if (!file.exists()) return emptyList()
            val arr = JSONArray(file.readText())
            val cutoff = System.currentTimeMillis() - 24L * 3600_000L
            val list = mutableListOf<HistoryPoint>()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val t = o.getLong("t")
                if (t < cutoff) continue
                list.add(
                    HistoryPoint(
                        time = t,
                        temp = if (o.isNull("c")) null else o.getDouble("c"),
                        setpoint = if (o.isNull("s")) null else o.getDouble("s"),
                        boilerOn = o.optBoolean("b", false)
                    )
                )
            }
            list
        } catch (e: Exception) {
            emptyList()
        }
    }

    fun save(points: List<HistoryPoint>) {
        try {
            val arr = JSONArray()
            for (p in points) {
                val o = JSONObject()
                o.put("t", p.time)
                o.put("c", p.temp ?: JSONObject.NULL)
                o.put("s", p.setpoint ?: JSONObject.NULL)
                o.put("b", p.boilerOn)
                arr.put(o)
            }
            file.writeText(arr.toString())
        } catch (e: Exception) {
        }
    }
}
