package com.garden.controller.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.garden.controller.GardenViewModel
import com.garden.controller.Link

@Composable
fun SettingsScreen(vm: GardenViewModel) {
    var host by remember { mutableStateOf(vm.settings.host) }
    var portText by remember { mutableStateOf(vm.settings.port.toString()) }
    var user by remember { mutableStateOf(vm.settings.username) }
    var pass by remember { mutableStateOf(vm.settings.password) }
    var base by remember { mutableStateOf(vm.settings.base) }
    var tls by remember { mutableStateOf(vm.settings.tls) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        InfoCard("MQTT-брокер") {
            OutlinedTextField(
                value = host, onValueChange = { host = it },
                label = { Text("Адреса (xxxx.emqxsl.com)") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth()
            )
            OutlinedTextField(
                value = portText, onValueChange = { portText = it.filter { c -> c.isDigit() }.take(5) },
                label = { Text("Порт") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth()
            )
            OutlinedTextField(
                value = user, onValueChange = { user = it },
                label = { Text("Користувач") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth()
            )
            OutlinedTextField(
                value = pass, onValueChange = { pass = it },
                label = { Text("Пароль") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth()
            )
            OutlinedTextField(
                value = base, onValueChange = { base = it },
                label = { Text("Базовий топік") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth()
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("TLS-шифрування")
                Switch(checked = tls, onCheckedChange = { tls = it })
            }
            Text(
                "Для EMQX Cloud: порт 8883 з TLS. Створіть для телефона окремого користувача з правами на топік garden/controller/#.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Button(
                onClick = {
                    val port = portText.toIntOrNull() ?: if (tls) 8883 else 1883
                    vm.apply(vm.settings.copy(host = host, port = port, username = user, password = pass, base = base, tls = tls))
                },
                enabled = host.isNotBlank(),
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Зберегти й підключити")
            }
        }

        InfoCard("Стан") {
            val (text, color) = when (vm.link) {
                Link.IDLE -> "Не підключено" to Color(0xFFEF6C00)
                Link.CONNECTING -> "Підключення…" to Color(0xFFEF6C00)
                Link.CONNECTED ->
                    if (vm.controllerOnline) "Підключено" to Color(0xFF2E7D32)
                    else "Контролер офлайн" to Color(0xFFEF6C00)
                Link.FAILED -> (vm.linkError ?: "Помилка") to Color(0xFFC62828)
            }
            Text(text, color = color, style = MaterialTheme.typography.titleMedium)
        }

        val s = vm.status
        if (s != null) {
            InfoCard("Контролер") {
                InfoRow("Прошивка", s.firmware)
                InfoRow("IP", s.ip)
                InfoRow("Wi‑Fi", s.wifiRssi?.let { "$it dBm" } ?: "–")
                InfoRow("Працює", uptimeText(s.uptimeS))
                InfoRow("Вільна пам'ять", "${s.freeHeap / 1024} КБ")
                InfoRow("Джерело температури", s.temperatureSource)
            }
        }

        OutlinedButton(onClick = { vm.clearHistory() }, modifier = Modifier.fillMaxWidth()) {
            Text("Очистити історію графіка")
        }
    }
}

@Composable
private fun InfoRow(title: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(title)
        Text(value, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

private fun uptimeText(sec: Long): String {
    val d = sec / 86400
    val h = (sec % 86400) / 3600
    val m = (sec % 3600) / 60
    return when {
        d > 0 -> "$d д $h год"
        h > 0 -> "$h год $m хв"
        else -> "$m хв"
    }
}
