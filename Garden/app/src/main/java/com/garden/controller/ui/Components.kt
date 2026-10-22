package com.garden.controller.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.garden.controller.GardenViewModel
import com.garden.controller.Link

@Composable
fun InfoCard(
    title: String? = null,
    content: @Composable ColumnScope.() -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            if (title != null) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            content()
        }
    }
}

/** Смуга стану з'єднання (показується лише коли є що повідомити). */
@Composable
fun ConnectionBanner(vm: GardenViewModel) {
    val problem: Boolean
    val text: String
    when (vm.link) {
        Link.IDLE -> { text = "Не підключено до брокера"; problem = false }
        Link.CONNECTING -> { text = "Підключення…"; problem = false }
        Link.FAILED -> { text = "Помилка: ${vm.linkError ?: "невідома"}"; problem = true }
        Link.CONNECTED -> {
            if (!vm.controllerOnline) {
                text = "Контролер офлайн"; problem = true
            } else if (vm.lastUpdate > 0 && vm.nowMs - vm.lastUpdate > 90_000) {
                text = "Дані застаріли"; problem = false
            } else {
                return
            }
        }
    }
    val container = if (problem) MaterialTheme.colorScheme.errorContainer else MaterialTheme.colorScheme.tertiaryContainer
    val onContainer = if (problem) MaterialTheme.colorScheme.onErrorContainer else MaterialTheme.colorScheme.onTertiaryContainer
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = container, contentColor = onContainer)
    ) {
        Row(
            modifier = Modifier.padding(start = 14.dp, end = 6.dp, top = 6.dp, bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(text, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            if (vm.link == Link.FAILED || vm.link == Link.IDLE) {
                TextButton(onClick = { vm.reconnect() }) { Text("Повторити") }
            }
        }
    }
}
