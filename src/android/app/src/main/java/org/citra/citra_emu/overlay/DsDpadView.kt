// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.overlay

import android.content.Context
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R

/**
 * A DS-only rebuild of [InputOverlayDrawableDpad]'s art and 8-way touch
 * logic. That class is wired into InputOverlay's drag-to-reposition/
 * per-game-layout/haptics machinery, which the DS player's overlay
 * doesn't have (see DsEmulationActivity.buildButtonOverlay) -- this
 * reuses the exact same dpad.png / dpad_pressed_one_direction.png /
 * dpad_pressed_two_directions.png assets so it looks identical to the
 * 3DS side's d-pad, with its own minimal single-pointer touch handling.
 */
class DsDpadView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {
    private val defaultDrawable =
        BitmapDrawable(resources, BitmapFactory.decodeResource(resources, R.drawable.dpad))
    private val oneDirectionDrawable = BitmapDrawable(
        resources,
        BitmapFactory.decodeResource(resources, R.drawable.dpad_pressed_one_direction)
    )
    private val twoDirectionsDrawable = BitmapDrawable(
        resources,
        BitmapFactory.decodeResource(resources, R.drawable.dpad_pressed_two_directions)
    )

    private var trackId = -1
    private var upPressed = false
    private var downPressed = false
    private var leftPressed = false
    private var rightPressed = false

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        defaultDrawable.setBounds(0, 0, w, h)
        oneDirectionDrawable.setBounds(0, 0, w, h)
        twoDirectionsDrawable.setBounds(0, 0, w, h)
    }

    override fun onDraw(canvas: Canvas) {
        val px = width / 2f
        val py = height / 2f
        when {
            upPressed && !leftPressed && !rightPressed -> oneDirectionDrawable.draw(canvas)
            downPressed && !leftPressed && !rightPressed ->
                rotatedDraw(canvas, 180f, px, py, oneDirectionDrawable)
            leftPressed && !upPressed && !downPressed ->
                rotatedDraw(canvas, 270f, px, py, oneDirectionDrawable)
            rightPressed && !upPressed && !downPressed ->
                rotatedDraw(canvas, 90f, px, py, oneDirectionDrawable)
            upPressed && leftPressed -> twoDirectionsDrawable.draw(canvas)
            upPressed && rightPressed -> rotatedDraw(canvas, 90f, px, py, twoDirectionsDrawable)
            downPressed && leftPressed -> rotatedDraw(canvas, 270f, px, py, twoDirectionsDrawable)
            downPressed && rightPressed -> rotatedDraw(canvas, 180f, px, py, twoDirectionsDrawable)
            else -> defaultDrawable.draw(canvas)
        }
    }

    private fun rotatedDraw(
        canvas: Canvas,
        degrees: Float,
        px: Float,
        py: Float,
        drawable: BitmapDrawable
    ) {
        canvas.save()
        canvas.rotate(degrees, px, py)
        drawable.draw(canvas)
        canvas.restore()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (trackId != -1) return true
                trackId = event.getPointerId(event.actionIndex)
                updateFromTouch(event, event.actionIndex)
            }
            MotionEvent.ACTION_MOVE -> {
                if (trackId == -1) return true
                val pointerIndex = event.findPointerIndex(trackId)
                if (pointerIndex == -1) return true
                updateFromTouch(event, pointerIndex)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) != trackId) return true
                trackId = -1
                release()
            }
            MotionEvent.ACTION_CANCEL -> {
                trackId = -1
                release()
            }
        }
        return true
    }

    private fun updateFromTouch(event: MotionEvent, pointerIndex: Int) {
        val x = event.getX(pointerIndex) - width / 2f
        val y = event.getY(pointerIndex) - height / 2f
        val maxX = width / 2f
        val maxY = height / 2f
        if (maxX <= 0f || maxY <= 0f) return
        setState(
            up = y / maxY < -DEADZONE,
            down = y / maxY > DEADZONE,
            left = x / maxX < -DEADZONE,
            right = x / maxX > DEADZONE
        )
    }

    private fun release() = setState(up = false, down = false, left = false, right = false)

    private fun setState(up: Boolean, down: Boolean, left: Boolean, right: Boolean) {
        if (up != upPressed) {
            upPressed = up
            NativeLibrary.dsOnButtonEvent(NativeLibrary.DsButtonType.DPAD_UP, up)
        }
        if (down != downPressed) {
            downPressed = down
            NativeLibrary.dsOnButtonEvent(NativeLibrary.DsButtonType.DPAD_DOWN, down)
        }
        if (left != leftPressed) {
            leftPressed = left
            NativeLibrary.dsOnButtonEvent(NativeLibrary.DsButtonType.DPAD_LEFT, left)
        }
        if (right != rightPressed) {
            rightPressed = right
            NativeLibrary.dsOnButtonEvent(NativeLibrary.DsButtonType.DPAD_RIGHT, right)
        }
        invalidate()
    }

    companion object {
        private const val DEADZONE = 0.4f
    }
}
