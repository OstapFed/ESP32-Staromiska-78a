package com.garden.controller.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.garden.controller.GardenViewModel
import com.garden.controller.data.fmt

@Composable
fun WaterScreen(vm: GardenViewModel) {
    val s = vm.status
    val raining = s?.rain ?: false
    val blocked = s?.rainBlocked ?: false
    val watering = s?.watering ?: false
    val activeZone = if (watering) (s?.wateringZone ?: 0) else 0

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        ConnectionBanner(vm)

        InfoCard("Стан") {
            val headline = when {
                raining -> "🌧 Іде дощ"
                blocked -> "⏳ Блокування після дощу"
                else -> "☀️ Сухо"
            }
            Text(headline, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            if (watering) {
                Text(
                    "💧 Триває полив, зона $activeZone",
                    color = MaterialTheme.colorScheme.secondary,
                    fontWeight = FontWeight.SemiBold
                )
            }
            if (s?.rainForecastMm != null) {
                val skip = if (s.weatherSkip) " — автополив буде пропущено" else ""
                Text(
                    "Прогноз на 6 год: ${fmt(s.rainForecastMm)} мм$skip",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }

        InfoCard("Запустити зону") {
            for (rowStart in listOf(1, 3)) {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    for (zone in rowStart..rowStart + 1) {
                        val active = activeZone == zone
                        Button(
                            onClick = { vm.startZone(zone) },
                            modifier = Modifier
                                .weight(1f)
                                .height(72.dp),
                            colors = if (active) {
                                ButtonDefaults.buttonColors()
                            } else {
                                ButtonDefaults.filledTonalButtonColors()
                            }
                        ) {
                            Text("💧 Зона $zone", style = MaterialTheme.typography.titleMedium)
                        }
                    }
                }
            }
        }

        Button(
            onClick = { vm.stopWatering() },
            modifier = Modifier
                .fillMaxWidth()
                .height(56.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFC62828), contentColor = Color.White)
        ) {
            Text("⏹ Зупинити полив", style = MaterialTheme.typography.titleMedium)
        }
    }
}
