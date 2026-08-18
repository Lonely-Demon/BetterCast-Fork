package com.bettercast.receiver.control

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View

internal class RemotePointerOverlay(context: Context) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(30, 136, 229)
        style = Paint.Style.FILL
        setShadowLayer(8f, 0f, 2f, Color.BLACK)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val radius = (width.coerceAtMost(height) * 0.28f).coerceAtLeast(8f)
        canvas.drawCircle(width / 2f, height / 2f, radius, paint)
    }
}
