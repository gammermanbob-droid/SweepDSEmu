// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.overlay

import android.content.Context
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import androidx.preference.PreferenceManager
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R

/**
 * A DS-only rebuild of [InputOverlay]'s button layer, reusing the exact
 * same [InputOverlayDrawableButton] class the 3DS side draws with (canvas
 * bounds filled edge-to-edge, no ImageButton default padding/minimum-
 * touch-target inset shrinking the artwork) plus its drag-to-reposition
 * support, rather than the plain [android.widget.ImageButton] instances
 * DsEmulationActivity.buildButtonOverlay previously used -- those shared
 * the same PNGs but still looked visibly different from the 3DS overlay
 * because ImageButton insets/scales its image within the platform
 * button style's own padding.
 *
 * Deliberately its own (much smaller) class rather than a direct reuse
 * of [InputOverlay] itself: that class is built entirely around the
 * 3DS's own button set (circle pad, C-stick, ZL/ZR, swap/turbo/combo)
 * and per-button scale menu, none of which apply here -- DS only has
 * the eight face/shoulder/start-select buttons this draws.
 */
class DsButtonOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private data class Spec(
        val id: Int,
        val prefKey: String,
        val normalRes: Int,
        val pressedRes: Int,
        val sizeDp: Int,
        val defaultPosition: (viewW: Int, viewH: Int, sizePx: Int) -> Pair<Int, Int>
    )

    private val prefs: SharedPreferences =
        PreferenceManager.getDefaultSharedPreferences(context)

    // Same dp constants and corner placements DsEmulationActivity's own
    // buildButtonOverlay used to lay these out with, just expressed as a
    // default-position function of this view's own size instead of
    // FrameLayout gravity+margins -- only used the first time a button
    // is drawn (i.e. no saved position yet); every touch/drag through
    // this class persists its own position afterward.
    private val specs: List<Spec> by lazy {
        val faceSize = 64
        val faceGap = 8
        listOf(
            Spec(NativeLibrary.DsButtonType.BUTTON_Y, "ds_overlay_y", R.drawable.button_y,
                R.drawable.button_y_pressed, faceSize) { w, _, s -> Pair(w - dp(faceSize + faceGap) - s, 0) },
            Spec(NativeLibrary.DsButtonType.BUTTON_A, "ds_overlay_a", R.drawable.button_a,
                R.drawable.button_a_pressed, faceSize) { w, _, s ->
                Pair(w - dp(2 * faceSize + 2 * faceGap) - s, dp(faceSize + faceGap))
            },
            Spec(NativeLibrary.DsButtonType.BUTTON_B, "ds_overlay_b", R.drawable.button_b,
                R.drawable.button_b_pressed, faceSize) { w, _, s -> Pair(w - s, dp(faceSize + faceGap)) },
            Spec(NativeLibrary.DsButtonType.BUTTON_X, "ds_overlay_x", R.drawable.button_x,
                R.drawable.button_x_pressed, faceSize) { w, _, s ->
                Pair(w - dp(faceSize + faceGap) - s, dp(2 * faceSize + 2 * faceGap))
            },
            Spec(NativeLibrary.DsButtonType.BUTTON_L, "ds_overlay_l", R.drawable.button_l,
                R.drawable.button_l_pressed, 56) { _, _, _ -> Pair(dp(16), dp(16)) },
            Spec(NativeLibrary.DsButtonType.BUTTON_R, "ds_overlay_r", R.drawable.button_r,
                R.drawable.button_r_pressed, 56) { w, _, s -> Pair(w - dp(16) - s, dp(16)) },
            Spec(NativeLibrary.DsButtonType.BUTTON_SELECT, "ds_overlay_select", R.drawable.button_select,
                R.drawable.button_select_pressed, 48) { _, h, s -> Pair(dp(16), h - dp(16) - s) },
            Spec(NativeLibrary.DsButtonType.BUTTON_START, "ds_overlay_start", R.drawable.button_start,
                R.drawable.button_start_pressed, 48) { w, h, s -> Pair(w - dp(16) - s, h - dp(16) - s) }
        )
    }

    private val buttons = mutableListOf<InputOverlayDrawableButton>()
    private var laidOut = false

    var isInEditMode = false
        set(value) {
            field = value
            invalidate()
        }
    private var buttonBeingConfigured: InputOverlayDrawableButton? = null

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun scaledBitmap(resId: Int, sizePx: Int): Bitmap {
        val original = BitmapFactory.decodeResource(resources, resId)
        return Bitmap.createScaledBitmap(original, sizePx, sizePx, true)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (laidOut || w <= 0 || h <= 0) return
        laidOut = true

        buttons.clear()
        for (spec in specs) {
            val sizePx = dp(spec.sizeDp)
            val button = InputOverlayDrawableButton(
                resources,
                scaledBitmap(spec.normalRes, sizePx),
                scaledBitmap(spec.pressedRes, sizePx),
                spec.id,
                217 // ~0.85 alpha, matching the previous ImageButton's alpha=0.85f
            )
            val savedX = prefs.getFloat("${spec.prefKey}_x", Float.NaN)
            val savedY = prefs.getFloat("${spec.prefKey}_y", Float.NaN)
            val (x, y) = if (!savedX.isNaN() && !savedY.isNaN()) {
                Pair(savedX.toInt(), savedY.toInt())
            } else {
                spec.defaultPosition(w, h, sizePx)
            }
            button.setBounds(x, y, x + sizePx, y + sizePx)
            button.setPosition(x, y)
            buttons.add(button)
        }
    }

    override fun onDraw(canvas: Canvas) {
        buttons.forEach { it.draw(canvas) }
    }

    /** Clears saved positions and re-lays-out at the built-in defaults. */
    fun resetPositions() {
        val editor = prefs.edit()
        for (spec in specs) {
            editor.remove("${spec.prefKey}_x")
            editor.remove("${spec.prefKey}_y")
        }
        editor.apply()
        laidOut = false
        onSizeChanged(width, height, width, height)
        invalidate()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (isInEditMode) {
            return onTouchWhileEditing(event)
        }

        val pointerIndex = event.actionIndex
        val action = event.action and MotionEvent.ACTION_MASK

        // This view spans the whole play area (MATCH_PARENT), overlaid
        // on top of DsDpadView -- unlike InputOverlay's own buttons,
        // which each occupy just their own small View, so there's
        // nothing for a dpad tap to "fall through" past on that side.
        // Returning true unconditionally here would swallow every
        // touch anywhere on screen, including ones meant for the dpad
        // underneath (FrameLayout hit-tests the most-recently-added,
        // i.e. topmost, child first) -- only claim a pointer when one
        // of *my own* buttons actually handled it, and return false
        // otherwise so the event continues on to whatever's beneath.
        //
        // updateStatus() already does its own bounds-contains (on down)
        // and trackId-matches-this-pointer (on up) checks internally,
        // returning false as a no-op for every button that isn't the
        // one this particular pointer event actually applies to -- safe
        // to just offer the event to all of them and only act on a
        // true return. Passing overlay=null skips haptics (see
        // InputOverlayDrawableButton's own updateStatus signature) --
        // not wiring that up for this first pass rather than pulling in
        // the real, 3DS-specific InputOverlay class just to reach its
        // hapticFeedback() call.
        var consumed = false
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                for (button in buttons) {
                    if (button.updateStatus(event, pointerIndex, false, null)) {
                        NativeLibrary.dsOnButtonEvent(button.id, true)
                        consumed = true
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                for (button in buttons) {
                    if (button.updateStatus(event, pointerIndex, false, null)) {
                        NativeLibrary.dsOnButtonEvent(button.id, false)
                        consumed = true
                    }
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                for (button in buttons) {
                    if (button.trackId != -1) {
                        button.trackId = -1
                        NativeLibrary.dsOnButtonEvent(button.id, false)
                    }
                }
            }
            // ACTION_MOVE isn't handled -- once a pointer's ACTION_DOWN
            // is claimed above, Android keeps routing that pointer's
            // subsequent move/up events to this view regardless of what
            // it returns for the move events themselves (only the
            // initial down's return value decides which view claims a
            // given pointer), so there's nothing to do here for the
            // simple press/release button model this view implements
            // (no button-sliding support, unlike InputOverlayDrawableButton's
            // own optional handling of that via updateStatus's move branch).
        }
        if (consumed) {
            invalidate()
        }
        return consumed
    }

    private fun onTouchWhileEditing(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex
        val x = event.getX(pointerIndex).toInt()
        val y = event.getY(pointerIndex).toInt()

        when (event.action and MotionEvent.ACTION_MASK) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                if (buttonBeingConfigured == null) {
                    buttons.firstOrNull { it.bounds.contains(x, y) }?.let {
                        buttonBeingConfigured = it
                        it.onConfigureTouch(event)
                    }
                }
            }
            MotionEvent.ACTION_MOVE -> {
                buttonBeingConfigured?.let {
                    it.onConfigureTouch(event)
                    invalidate()
                    return true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                buttonBeingConfigured?.let {
                    val spec = specs.first { spec -> spec.id == it.id }
                    prefs.edit()
                        .putFloat("${spec.prefKey}_x", it.bounds.left.toFloat())
                        .putFloat("${spec.prefKey}_y", it.bounds.top.toFloat())
                        .apply()
                    buttonBeingConfigured = null
                }
            }
        }
        return true
    }
}
