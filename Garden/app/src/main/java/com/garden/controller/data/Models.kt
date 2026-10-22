package com.garden.controller.data

import org.json.JSONObject
import java.util.Locale

data class Room(
    val enabled: Boolean,
    val name: String,
    val online: Boolean,
    val temperature: Double?,
    val humidity: Double?,
    val battery: Int?
)

/** Стан контролера: JSON з топіка <base>/state (формат /api/status). */
data class GardenStatus(
    val temperature: Double?,
    val boilerOn: Boolean,
    val boilerMode: Int,
    val boilerSetpoint: Double?,
    val vacation: Boolean,
    val lights: List<Boolean>,
    val watering: Boolean,
    val wateringZone: Int,
    val rain: Boolean,
    val rainBlocked: Boolean,
    val rainForecastMm: Double?,
    val weatherSkip: Boolean,
    val outdoorTemperature: Double?,
    val rooms: List<Room>,
    val wifiRssi: Int?,
    val firmware: String,
    val uptimeS: Long,
    val freeHeap: Long,
    val ip: String,
    val temperatureSource: String
) {
    companion object {
        fun parse(text: String): GardenStatus? = try {
            from(JSONObject(text))
        } catch (e: Exception) {
            null
        }

        private fun JSONObject.dbl(key: String): Double? =
            if (has(key) && !isNull(key)) optDouble(key) else null

        private fun JSONObject.intOrNull(key: String): Int? =
            if (has(key) && !isNull(key)) optInt(key) else null

        private fun from(o: JSONObject): GardenStatus {
            val rooms = mutableListOf<Room>()
            val sensors = o.optJSONArray("xiaomi_sensors")
            if (sensors != null) {
                for (i in 0 until sensors.length()) {
                    val r = sensors.optJSONObject(i) ?: continue
                    rooms.add(
                        Room(
                            enabled = r.optBoolean("enabled", false),
                            name = r.optString("name", "Датчик"),
                            online = r.optBoolean("online", false),
                            temperature = r.dbl("temperature"),
                            humidity = r.dbl("humidity"),
                            battery = r.intOrNull("battery")
                        )
                    )
                }
            }
            val lights = mutableListOf<Boolean>()
            val la = o.optJSONArray("lights")
            if (la != null) for (i in 0 until la.length()) lights.add(la.optBoolean(i, false))

            return GardenStatus(
                temperature = o.dbl("temperature"),
                boilerOn = o.optBoolean("boiler_on", false),
                boilerMode = o.optInt("boiler_mode", 2),
                boilerSetpoint = o.dbl("boiler_setpoint"),
                vacation = o.optBoolean("vacation", false),
                lights = lights,
                watering = o.optBoolean("watering", false),
                wateringZone = o.optInt("watering_zone", 0),
                rain = o.optBoolean("rain", false),
                rainBlocked = o.optBoolean("rain_blocked", false),
                rainForecastMm = o.dbl("rain_forecast_mm"),
                weatherSkip = o.optBoolean("weather_skip", false),
                outdoorTemperature = o.dbl("outdoor_temperature"),
                rooms = rooms,
                wifiRssi = o.intOrNull("wifi_rssi"),
                firmware = o.optString("firmware", "–"),
                uptimeS = o.optLong("uptime_s", 0),
                freeHeap = o.optLong("free_heap", 0),
                ip = o.optString("ip", "–"),
                temperatureSource = o.optString("temperature_source", "–")
            )
        }
    }
}

data class HistoryPoint(
    val time: Long,
    val temp: Double?,
    val setpoint: Double?,
    val boilerOn: Boolean
)

data class BrokerSettings(
    val host: String = "",
    val port: Int = 8883,
    val username: String = "",
    val password: String = "",
    val base: String = "garden/controller",
    val tls: Boolean = true
) {
    val isValid: Boolean get() = host.isNotBlank() && port in 1..65535 && base.isNotBlank()
}

fun fmt(v: Double?, digits: Int = 1): String =
    if (v == null) "–" else String.format(Locale.getDefault(), "%.${digits}f", v)
