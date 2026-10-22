package com.garden.controller

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.garden.controller.ui.GardenApp
import com.garden.controller.ui.GardenTheme

class MainActivity : ComponentActivity() {
    private val vm: GardenViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        lifecycle.addObserver(LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_START -> vm.resume()
                Lifecycle.Event.ON_STOP -> vm.pause()
                else -> Unit
            }
        })
        setContent {
            GardenTheme {
                GardenApp(vm)
            }
        }
    }
}
