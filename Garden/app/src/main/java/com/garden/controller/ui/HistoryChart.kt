package com.garden.controller.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp
import com.garden.controller.data.HistoryPoint
import com.garden.controller.data.fmt

/** Простий лінійний графік: температура (суцільна) та уставка (пунктир). */
@Composable
fun HistoryChart(points: List<HistoryPoint>, modifier: Modifier = Modifier) {
    val values = points.flatMap { listOfNotNull(it.temp, it.setpoint) }
    if (points.size < 2 || values.isEmpty()) return

    val minV = (values.minOrNull() ?: 0.0) - 0.5
    val maxV = (values.maxOrNull() ?: 1.0) + 0.5
    val t0 = points.first().time
    val t1 = points.last().time
    val span = (t1 - t0).coerceAtLeast(1L).toDouble()
    val tempColor = Color(0xFFFF9800)
    val setColor = Color(0xFF2879B8)

    Column(modifier = modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text("▬ Температура", color = tempColor, style = MaterialTheme.typography.labelMedium)
            Text("┅ Уставка", color = setColor, style = MaterialTheme.typography.labelMedium)
            Text("${fmt(minV)} … ${fmt(maxV)} °C", style = MaterialTheme.typography.labelMedium)
        }
        Canvas(modifier = Modifier.fillMaxWidth().height(170.dp)) {
            val w = size.width
            val h = size.height
            fun x(t: Long) = ((t - t0) / span * w).toFloat()
            fun y(v: Double) = (h - ((v - minV) / (maxV - minV) * h)).toFloat()

            // горизонтальні лінії сітки
            val grid = Color.Gray.copy(alpha = 0.25f)
            for (i in 0..3) {
                val gy = h * i / 3f
                drawLine(grid, Offset(0f, gy), Offset(w, gy), strokeWidth = 1f)
            }

            fun line(selector: (HistoryPoint) -> Double?, color: Color, dashed: Boolean) {
                val path = Path()
                var started = false
                for (p in points) {
                    val v = selector(p) ?: continue
                    val px = x(p.time)
                    val py = y(v)
                    if (!started) {
                        path.moveTo(px, py)
                        started = true
                    } else {
                        path.lineTo(px, py)
                    }
                }
                if (started) {
                    drawPath(
                        path = path,
                        color = color,
                        style = Stroke(
                            width = if (dashed) 3f else 5f,
                            pathEffect = if (dashed) PathEffect.dashPathEffect(floatArrayOf(14f, 10f)) else null
                        )
                    )
                }
            }
            line({ it.setpoint }, setColor, true)
            line({ it.temp }, tempColor, false)
        }
    }
}
