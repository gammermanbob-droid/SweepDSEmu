// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.overlay

import android.content.Context
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import androidx.preference.PreferenceManager
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.features.settings.model.Settings

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
 *
 * Default positions come from "housing zones" -- see
 * DsEmulationActivity.layoutDsScreens() -- rather than fixed corner
 * margins, since the DS screens are no longer stretched to fill the
 * whole display and buttons need to default into whatever's actually
 * left over on either side instead of sitting on top of the screens.
 */
class DsButtonOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private enum class Zone {
        LEFT, RIGHT,
        // L/R/Select/Start default to small corner floaters against the
        // *full* overlay (like the HOME button always has), not housed
        // inside the left/right squares -- those squares are sized for
        // the d-pad and face-button diamond alone; fitting four more
        // buttons in without overlapping either of those would mean
        // shrinking everything to the point of being fiddly to tap.
        FULL
    }

    private data class Spec(
        val id: Int,
        val prefKey: String,
        val normalRes: Int,
        val pressedRes: Int,
        val zone: Zone,
        // Fraction of the reference area's own (square, for LEFT/RIGHT;
        // shorter side, for FULL) side length -- scales with whatever
        // room is actually available instead of a fixed dp size, since
        // both zone size and window size vary a lot.
        val sizeFraction: Float,
        // Fraction of the reference area's width/height for the
        // button's top-left corner, 0..1 -- e.g. (0.5f - sizeFraction/2, 0f)
        // centers a button horizontally flush against the top edge.
        val position: (sizeFraction: Float) -> Pair<Float, Float>
    )

    private val specs: List<Spec> = listOf(
        Spec(NativeLibrary.DsButtonType.BUTTON_L, "ds_overlay_l", R.drawable.button_l,
            R.drawable.button_l_pressed, Zone.FULL, 0.09f) { s -> Pair(0.02f, 0.02f) },
        Spec(NativeLibrary.DsButtonType.BUTTON_R, "ds_overlay_r", R.drawable.button_r,
            R.drawable.button_r_pressed, Zone.FULL, 0.09f) { s -> Pair(1f - 0.02f - s, 0.02f) },
        Spec(NativeLibrary.DsButtonType.BUTTON_SELECT, "ds_overlay_select", R.drawable.button_select,
            R.drawable.button_select_pressed, Zone.FULL, 0.08f) { s -> Pair(0.02f, 1f - 0.02f - s) },
        Spec(NativeLibrary.DsButtonType.BUTTON_START, "ds_overlay_start", R.drawable.button_start,
            R.drawable.button_start_pressed, Zone.FULL, 0.08f) { s -> Pair(1f - 0.02f - s, 1f - 0.02f - s) },
        // Face buttons: a diamond centered in the right zone, same
        // relative Y-top/A-left/B-right/X-bottom shape the very first
        // version of this overlay used (see git history) -- kept as-is
        // rather than "corrected" to a real DS's actual X-top/Y-left/
        // A-right/B-bottom layout, since changing established button
        // positions wasn't asked for and would just retrain muscle memory
        // for no real benefit.
        Spec(NativeLibrary.DsButtonType.BUTTON_Y, "ds_overlay_y", R.drawable.button_y,
            R.drawable.button_y_pressed, Zone.RIGHT, 0.30f) { s -> Pair(0.5f - s / 2f, 0.5f - 0.22f - s / 2f) },
        Spec(NativeLibrary.DsButtonType.BUTTON_A, "ds_overlay_a", R.drawable.button_a,
            R.drawable.button_a_pressed, Zone.RIGHT, 0.30f) { s -> Pair(0.5f - 0.22f - s / 2f, 0.5f - s / 2f) },
        Spec(NativeLibrary.DsButtonType.BUTTON_B, "ds_overlay_b", R.drawable.button_b,
            R.drawable.button_b_pressed, Zone.RIGHT, 0.30f) { s -> Pair(0.5f + 0.22f - s / 2f, 0.5f - s / 2f) },
        Spec(NativeLibrary.DsButtonType.BUTTON_X, "ds_overlay_x", R.drawable.button_x,
            R.drawable.button_x_pressed, Zone.RIGHT, 0.30f) { s -> Pair(0.5f - s / 2f, 0.5f + 0.22f - s / 2f) }
    )

    private val prefs: SharedPreferences =
        PreferenceManager.getDefaultSharedPreferences(context)

    private val buttons = mutableListOf<InputOverlayDrawableButton>()
    private var leftZone = Rect()
    private var rightZone = Rect()
    private var zonesKnown = false

    var repositionModeEnabled = false
        set(value) {
            field = value
            invalidate()
        }
    private var buttonBeingConfigured: InputOverlayDrawableButton? = null
    // Set instead of buttonBeingConfigured when a touch starts on a
    // button's resize handle (see resizeHandleRect) rather than its
    // body -- the two are mutually exclusive per gesture.
    private var buttonBeingResized: InputOverlayDrawableButton? = null

    // Edit mode has to be visually unmistakable -- a translucent tint
    // plus a border around each housing zone -- since a silent mode
    // toggle with no on-screen feedback makes it impossible to tell
    // whether dragging isn't working or edit mode just never actually
    // turned on in the first place.
    private val editTintPaint = Paint().apply { color = Color.argb(60, 0, 150, 255) }
    private val editBorderPaint = Paint().apply {
        color = Color.argb(200, 0, 150, 255)
        style = Paint.Style.STROKE
        strokeWidth = 4f
    }
    private val resizeHandleFillPaint = Paint().apply { color = Color.argb(230, 255, 200, 0) }
    private val resizeHandleBorderPaint = Paint().apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = 2f
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    /**
     * Mirrors DsEmulationActivity's own remapFaceButton: applies the
     * Settings.KEY_DS_SWAP_AB / KEY_DS_SWAP_XY preferences so tapping the
     * on-screen button labeled "A" sends whichever bit the user actually
     * wants there, matching the same swap a physical gamepad gets. Not
     * shared code with that Activity (this View has no reference to it)
     * -- duplicated rather than plumbing a callback through for four
     * lines of logic.
     */
    private fun remapFaceButton(bit: Int): Int {
        if (prefs.getBoolean(Settings.KEY_DS_SWAP_AB, false)) {
            when (bit) {
                NativeLibrary.DsButtonType.BUTTON_A -> return NativeLibrary.DsButtonType.BUTTON_B
                NativeLibrary.DsButtonType.BUTTON_B -> return NativeLibrary.DsButtonType.BUTTON_A
            }
        }
        if (prefs.getBoolean(Settings.KEY_DS_SWAP_XY, false)) {
            when (bit) {
                NativeLibrary.DsButtonType.BUTTON_X -> return NativeLibrary.DsButtonType.BUTTON_Y
                NativeLibrary.DsButtonType.BUTTON_Y -> return NativeLibrary.DsButtonType.BUTTON_X
            }
        }
        return bit
    }

    // A drag handle at each button's bottom-right corner, only shown/
    // hit-testable in edit mode -- dragging the button's *body* moves
    // it (existing behavior), dragging this smaller square instead
    // resizes it, growing/shrinking from the fixed top-left corner
    // like a standard corner-resize handle in an image/window editor.
    // Deliberately not a pinch gesture: this project's touch-dispatch
    // code has had more than one subtle multi-pointer bug already this
    // session, and a single-finger drag on a small, precisely-defined
    // hit target is far more predictable to get right without being
    // able to test on-device myself.
    private val resizeHandleSizePx get() = dp(28)

    private fun resizeHandleRect(button: InputOverlayDrawableButton): Rect {
        val h = resizeHandleSizePx
        return Rect(button.bounds.right - h, button.bounds.bottom - h, button.bounds.right, button.bounds.bottom)
    }

    private fun scaledBitmap(resId: Int, sizePx: Int): Bitmap {
        val original = BitmapFactory.decodeResource(resources, resId)
        return Bitmap.createScaledBitmap(original, sizePx.coerceAtLeast(1), sizePx.coerceAtLeast(1), true)
    }

    /**
     * Called by DsEmulationActivity.layoutDsScreens() whenever the
     * screen layout (and therefore the housing zones) is computed or
     * recomputed. Buttons with a saved dragged position keep it;
     * buttons without one are (re-)placed using [Spec.position] against
     * whichever zone actually changed size, so rotating the device
     * doesn't silently undo a drag.
     */
    fun setHousingZones(left: Rect, right: Rect) {
        if (zonesKnown && left == leftZone && right == rightZone) {
            return // no actual change -- avoid redundant relayout on every layout pass
        }
        leftZone = left
        rightZone = right
        zonesKnown = true

        // FULL specs (L/R/Select/Start) reference the whole overlay's
        // own bounds rather than either housing square -- this view is
        // already MATCH_PARENT, so its own width/height (available once
        // it's been laid out at least once, which it always has been by
        // the time setHousingZones() first runs from layoutDsScreens())
        // is exactly that reference area.
        val fullZone = Rect(0, 0, width, height)

        buttons.clear()
        for (spec in specs) {
            val zone = when (spec.zone) {
                Zone.LEFT -> leftZone
                Zone.RIGHT -> rightZone
                Zone.FULL -> fullZone
            }
            val defaultSizePx = (zone.width().coerceAtMost(zone.height()) * spec.sizeFraction).toInt()
            val savedSize = prefs.getFloat("${spec.prefKey}_size", Float.NaN)
            val sizePx = if (!savedSize.isNaN()) savedSize.toInt() else defaultSizePx

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
                val (fx, fy) = spec.position(spec.sizeFraction)
                Pair(
                    zone.left + (zone.width() * fx).toInt(),
                    zone.top + (zone.height() * fy).toInt()
                )
            }
            button.setBounds(x, y, x + sizePx, y + sizePx)
            button.setPosition(x, y)
            buttons.add(button)
        }
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (repositionModeEnabled) {
            canvas.drawRect(leftZone, editTintPaint)
            canvas.drawRect(leftZone, editBorderPaint)
            canvas.drawRect(rightZone, editTintPaint)
            canvas.drawRect(rightZone, editBorderPaint)
        }
        buttons.forEach { it.draw(canvas) }
        if (repositionModeEnabled) {
            for (button in buttons) {
                val handle = resizeHandleRect(button)
                canvas.drawRect(handle, resizeHandleFillPaint)
                canvas.drawRect(handle, resizeHandleBorderPaint)
            }
        }
    }

    /** Clears saved positions/sizes and re-lays-out at the built-in defaults. */
    fun resetPositions() {
        val editor = prefs.edit()
        for (spec in specs) {
            editor.remove("${spec.prefKey}_x")
            editor.remove("${spec.prefKey}_y")
            editor.remove("${spec.prefKey}_size")
        }
        editor.apply()
        zonesKnown = false
        setHousingZones(leftZone, rightZone)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (repositionModeEnabled) {
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
                        NativeLibrary.dsOnButtonEvent(remapFaceButton(button.id), true)
                        consumed = true
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                for (button in buttons) {
                    if (button.updateStatus(event, pointerIndex, false, null)) {
                        NativeLibrary.dsOnButtonEvent(remapFaceButton(button.id), false)
                        consumed = true
                    }
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                for (button in buttons) {
                    if (button.trackId != -1) {
                        button.trackId = -1
                        NativeLibrary.dsOnButtonEvent(remapFaceButton(button.id), false)
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
                if (buttonBeingConfigured == null && buttonBeingResized == null) {
                    // Resize handle takes priority: it's a small region
                    // at the button's own bottom-right corner, so a tap
                    // there would also satisfy the body's own (larger)
                    // bounds.contains() check below -- check it first or
                    // the handle would never actually be reachable.
                    val resizeTarget = buttons.firstOrNull { resizeHandleRect(it).contains(x, y) }
                    if (resizeTarget != null) {
                        buttonBeingResized = resizeTarget
                        invalidate()
                    } else {
                        buttons.firstOrNull { it.bounds.contains(x, y) }?.let {
                            buttonBeingConfigured = it
                            it.onConfigureTouch(event)
                            invalidate()
                        }
                    }
                }
            }
            MotionEvent.ACTION_MOVE -> {
                buttonBeingResized?.let {
                    // Anchor (top-left) stays fixed; size tracks however
                    // far the touch has moved right/down from it, same
                    // convention as a corner-resize handle in an image or
                    // window editor -- whichever axis moved further
                    // decides the (always-square) new size, so a mostly-
                    // horizontal or mostly-vertical drag both feel natural.
                    val anchorX = it.bounds.left
                    val anchorY = it.bounds.top
                    val newSize = maxOf(x - anchorX, y - anchorY)
                        .coerceIn(resizeHandleSizePx, dp(MAX_BUTTON_SIZE_DP))
                    it.setBounds(anchorX, anchorY, anchorX + newSize, anchorY + newSize)
                    invalidate()
                    return true
                }
                buttonBeingConfigured?.let {
                    it.onConfigureTouch(event)
                    invalidate()
                    return true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                buttonBeingResized?.let {
                    val spec = specs.first { spec -> spec.id == it.id }
                    prefs.edit().putFloat("${spec.prefKey}_size", it.bounds.width().toFloat()).apply()
                    buttonBeingResized = null
                    // Resizing changes the bitmap's own intrinsic size, not
                    // just its drawn bounds -- setBounds() during the drag
                    // just stretches the original-resolution bitmap
                    // (softer, but fine for a live preview); re-run the
                    // full layout once to decode it fresh at the final
                    // size so it's crisp once the gesture is done.
                    zonesKnown = false
                    setHousingZones(leftZone, rightZone)
                }
                buttonBeingConfigured?.let {
                    val spec = specs.first { spec -> spec.id == it.id }
                    prefs.edit()
                        .putFloat("${spec.prefKey}_x", it.bounds.left.toFloat())
                        .putFloat("${spec.prefKey}_y", it.bounds.top.toFloat())
                        .apply()
                    buttonBeingConfigured = null
                    invalidate()
                }
            }
        }
        return true
    }

    companion object {
        private const val MAX_BUTTON_SIZE_DP = 260
    }
}
