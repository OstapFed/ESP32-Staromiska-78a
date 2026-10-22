package com.garden.controller.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.garden.controller.GardenViewModel

@Composable
fun LightsScreen(vm: GardenViewModel) {
    val lights = vm.status?.lights ?: emptyList()
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        ConnectionBanner(vm)
        InfoCard("Освітлення") {
            if (lights.isEmpty()) {
                Text("Немає даних від контролера", color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                lights.forEachIndexed { index, on ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = (if (on) "💡 " else "▫️ ") + "Канал ${index + 1}",
                            style = MaterialTheme.typography.titleMedium
                        )
                        Switch(checked = on, onCheckedChange = { vm.setLight(index, it) })
                    }
                }
            }
        }
    }
}
