package com.garden.controller.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.garden.controller.GardenViewModel
import com.garden.controller.data.fmt

@Composable
fun DashboardScreen(vm: GardenViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        ConnectionBanner(vm)
        HeatingCard(vm)
        RoomsCard(vm)
        HistoryCard(vm)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HeatingCard(vm: GardenViewModel) {
    val s = vm.status
    val boilerOn = s?.boilerOn ?: false
    val vacation = s?.vacation ?: false
    val mode = s?.boilerMode ?: 2

    InfoCard("Опалення") {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = fmt(s?.temperature) + "°",
                fontSize = 56.sp,
                fontWeight = FontWeight.SemiBold
            )
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    text = if (boilerOn) "🔥 Котел працює" else "Котел вимкнено",
                    fontWeight = FontWeight.SemiBold,
                    color = if (boilerOn) Color(0xFFFF9800) else MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (s?.outdoorTemperature != null) {
                    Text(
                        text = "надворі ${fmt(s.outdoorTemperature)}°",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            FilledTonalIconButton(onClick = { vm.adjustSetpoint(-0.5) }, modifier = Modifier.size(56.dp)) {
                Text("−", fontSize = 28.sp)
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    text = if (vacation) "Уставка відпустки" else "Уставка",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    text = fmt(vm.targetSetpoint ?: s?.boilerSetpoint) + " °C",
                    fontSize = 30.sp,
                    fontWeight = FontWeight.Bold
                )
            }
            FilledTonalIconButton(onClick = { vm.adjustSetpoint(0.5) }, modifier = Modifier.size(56.dp)) {
                Text("+", fontSize = 28.sp)
            }
        }

        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf(0 to "Вимк", 1 to "Ручний", 2 to "Авто").forEach { (value, label) ->
                FilterChip(
                    selected = mode == value,
                    onClick = { vm.setMode(value) },
                    label = { Text(label) }
                )
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("🏖 Режим «Відпустка»", style = MaterialTheme.typography.bodyLarge)
            Switch(checked = vacation, onCheckedChange = { vm.setVacation(it) })
        }

        OutlinedButton(onClick = { vm.resetToSchedule() }, modifier = Modifier.fillMaxWidth()) {
            Text("Повернутись до розкладу")
        }
    }
}

@Composable
private fun RoomsCard(vm: GardenViewModel) {
    val rooms = vm.status?.rooms?.filter { it.enabled } ?: emptyList()
    if (rooms.isEmpty()) return
    InfoCard("Кімнати") {
        rooms.forEach { room ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .background(if (room.online) Color(0xFF2E7D32) else Color(0xFFC62828), CircleShape)
                )
                Spacer(Modifier.size(10.dp))
                Text(room.name, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
                Column(horizontalAlignment = Alignment.End) {
                    Text(fmt(room.temperature) + " °C", fontWeight = FontWeight.SemiBold)
                    Text(
                        text = "${fmt(room.humidity, 0)}% · 🔋${room.battery?.toString() ?: "–"}%",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HistoryCard(vm: GardenViewModel) {
    var hours by remember { mutableIntStateOf(6) }
    val cutoff = vm.nowMs - hours * 3600_000L
    val pts = vm.history.filter { it.time >= cutoff }

    InfoCard("Температура") {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf(1, 6, 24).forEach { h ->
                FilterChip(
                    selected = hours == h,
                    onClick = { hours = h },
                    label = { Text("$h год") }
                )
            }
        }
        if (pts.size < 2) {
            Text(
                "Дані з'являться через кілька хвилин роботи додатка.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        } else {
            HistoryChart(pts, Modifier.fillMaxWidth())
        }
    }
}
