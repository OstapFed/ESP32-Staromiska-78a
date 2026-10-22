package com.garden.controller.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.garden.controller.GardenViewModel

private data class TabItem(val icon: String, val title: String)

private val tabs = listOf(
    TabItem("🏠", "Дім"),
    TabItem("💧", "Полив"),
    TabItem("💡", "Світло"),
    TabItem("⚙️", "Налашт.")
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GardenApp(vm: GardenViewModel) {
    var tab by rememberSaveable { mutableStateOf(0) }
    val snackbar = remember { SnackbarHostState() }
    val toast = vm.toast

    LaunchedEffect(toast) {
        if (toast != null) snackbar.showSnackbar(toast)
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        bottomBar = {
            NavigationBar {
                tabs.forEachIndexed { index, item ->
                    NavigationBarItem(
                        selected = tab == index,
                        onClick = { tab = index },
                        icon = { Text(item.icon) },
                        label = { Text(item.title) }
                    )
                }
            }
        }
    ) { inner ->
        Box(modifier = Modifier.padding(inner).fillMaxSize()) {
            when (tab) {
                0 -> DashboardScreen(vm)
                1 -> WaterScreen(vm)
                2 -> LightsScreen(vm)
                else -> SettingsScreen(vm)
            }
        }
    }
}
