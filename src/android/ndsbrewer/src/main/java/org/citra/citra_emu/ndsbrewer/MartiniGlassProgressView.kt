// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import android.view.animation.LinearInterpolator
import androidx.core.graphics.ColorUtils

/**
 * NDSBrewer's build-progress indicator: a martini glass that fills up as
 * ROMs are processed, in place of a plain ProgressBar/LinearProgressIndicator.
 * Mirrors ProgressBar's max/progress API so it drops into MainActivity's
 * existing progress-dialog code unchanged.
 */
class MartiniGlassProgressView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var max: Int = 100
        set(value) {
            field = value.coerceAtLeast(1)
            invalidate()
        }

    var progress: Int = 0
        set(value) {
            field = value.coerceIn(0, max)
            invalidate()
        }

    private val fraction: Float
        get() = progress.toFloat() / max.toFloat()

    private val glassPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 5f
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private val liquidPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val highlightPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val oliveePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val pickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
        strokeCap = Paint.Cap.ROUND
    }

    private val glassColor = ColorUtils.setAlphaComponent(0xFFFFFF, 130)
    private val liquidTop = 0xFFE8E37A.toInt()
    private val liquidBottom = 0xFFB7C24A.toInt()

    private var waveShift = 0f
    private val waveAnimator = ValueAnimator.ofFloat(0f, 2f * Math.PI.toFloat()).apply {
        duration = 2600
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener {
            waveShift = it.animatedValue as Float
            invalidate()
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        waveAnimator.start()
    }

    override fun onDetachedFromWindow() {
        waveAnimator.cancel()
        super.onDetachedFromWindow()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val desiredHeight = (width * 0.62f).toInt().coerceIn(dp(96), dp(180))
        val height = resolveSize(desiredHeight, heightMeasureSpec)
        setMeasuredDimension(width, height)
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        val marginX = w * 0.16f
        val rimY = h * 0.08f
        val apexY = h * 0.56f
        val stemBottomY = h * 0.82f
        val baseY = h * 0.90f
        val centerX = w / 2f

        // Bowl (inverted triangle), stem, and base -- drawn as one outline path.
        val bowl = Path().apply {
            moveTo(marginX, rimY)
            lineTo(w - marginX, rimY)
            lineTo(centerX, apexY)
            close()
        }
        val stem = Path().apply {
            moveTo(centerX, apexY)
            lineTo(centerX, stemBottomY)
        }
        val baseWidth = w * 0.32f
        val base = Path().apply {
            moveTo(centerX - baseWidth / 2f, baseY)
            lineTo(centerX + baseWidth / 2f, baseY)
        }

        // Liquid: bowl interior clipped above a level line that rises with progress.
        if (fraction > 0f) {
            val levelY = apexY - (apexY - rimY) * fraction
            val liquidPath = Path(bowl)
            canvas.save()
            canvas.clipPath(liquidPath)
            canvas.clipRect(0f, levelY, w, apexY)
            liquidPaint.shader = LinearGradient(
                0f, levelY, 0f, apexY, liquidTop, liquidBottom, Shader.TileMode.CLAMP
            )
            canvas.drawRect(0f, levelY, w, apexY, liquidPaint)

            // A gentle sine wave along the liquid surface for a bit of life.
            val wavePath = Path().apply {
                moveTo(0f, levelY)
                val steps = 24
                for (i in 0..steps) {
                    val x = w * i / steps
                    val y = levelY + sinePx(x, w)
                    lineTo(x, y)
                }
                lineTo(w, apexY)
                lineTo(0f, apexY)
                close()
            }
            highlightPaint.color = ColorUtils.setAlphaComponent(0xFFFFFF, 60)
            canvas.drawPath(wavePath, highlightPaint)
            canvas.restore()

            // Olive garnish once the build is complete.
            if (progress >= max) {
                val oliveCx = centerX + w * 0.05f
                val oliveCy = (levelY + rimY) / 2f
                val oliveR = w * 0.045f
                oliveePaint.color = 0xFF6B8E23.toInt()
                canvas.drawCircle(oliveCx, oliveCy, oliveR, oliveePaint)
                oliveePaint.color = 0xFFB33A3A.toInt()
                canvas.drawCircle(oliveCx, oliveCy, oliveR * 0.28f, oliveePaint)
                pickPaint.color = 0xFFD9B36C.toInt()
                canvas.drawLine(oliveCx - oliveR, oliveCy, w - marginX * 0.6f, rimY - h * 0.02f, pickPaint)
            }
        }

        canvas.drawPath(bowl, glassPaint.apply { color = glassColor })
        canvas.drawPath(stem, glassPaint)
        canvas.drawPath(base, glassPaint)
    }

    private fun sinePx(x: Float, w: Float): Float {
        val amplitude = h() * 0.012f
        return (Math.sin((x / w) * 4 * Math.PI + waveShift) * amplitude).toFloat()
    }

    private fun h(): Float = height.toFloat()
}
