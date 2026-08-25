// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.activities

import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageButton
import androidx.annotation.DrawableRes
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.databinding.ActivityDsEmulationBinding
import org.citra.citra_emu.display.SecondaryDisplay
import org.citra.citra_emu.overlay.DsDpadView

/**
 * Plays a DS/DSi ROM via MergedCore::MelonDsCore (see core/melonds_core/),
 * parallel to [EmulationActivity]'s 3DS path. Reached either directly (a
 * .nds/.dsi file picked from the game list) or indirectly, when a DS
 * forwarder CIA's 3DS stub redirects here (see
 * NativeLibrary.launchDsForwarder and native.cpp's run()).
 *
 * Unlike EmulationActivity, DS frames arrive as plain RGBA8888 pixel
 * buffers from the native run thread (jni/ds_native.cpp), which blits
 * them directly via ANativeWindow -- no GL/Vulkan context or Choreographer
 * frame-pump is needed here, just two SurfaceViews to hand their
 * ANativeWindows to native code.
 */
class DsEmulationActivity : AppCompatActivity() {
    private lateinit var binding: ActivityDsEmulationBinding
    private lateinit var secondaryDisplayManager: SecondaryDisplay
    private var romPath: String = ""
    private var runThread: Thread? = null
    private var isRunning = false

    // True once a genuine second physical display is actually in use for
    // the bottom screen (dual-screen handhelds like the AYN Thor or Odin
    // 2 -- see SecondaryDisplay's doc comment). The DS's own two-screen
    // layout maps onto that kind of hardware directly: top screen on the
    // primary display, bottom (touch) screen on the real second one,
    // instead of splitting a single display in two like the fallback
    // stacked layout below does. Unlike EmulationActivity's fully
    // user-configurable multi-layout system (top/bottom/hybrid/side-by-
    // side, a per-setting choice backed by native screen-layout code),
    // this is a single sensible default with no configuration UI --
    // scoped down deliberately rather than rebuilding that whole system
    // for DS.
    private val usingSecondaryDisplay: Boolean
        get() = secondaryDisplayManager.currentDisplayId != -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityDsEmulationBinding.inflate(layoutInflater)
        setContentView(binding.root)

        romPath = intent.getStringExtra(EXTRA_DS_ROM_PATH) ?: ""
        if (romPath.isEmpty()) {
            finish()
            return
        }

        NativeLibrary.sDsEmulationActivity = java.lang.ref.WeakReference(this)

        secondaryDisplayManager = SecondaryDisplay(
            this,
            onSurfaceUpdate = { NativeLibrary.dsBottomSurfaceChanged(it) },
            onSurfaceDestroy = { NativeLibrary.dsBottomSurfaceDestroyed() },
            onTouch = { x, y, pressed -> onSecondaryDisplayTouch(x, y, pressed) },
            onTouchMoved = { x, y -> onSecondaryDisplayTouch(x, y, true) }
        )
        secondaryDisplayManager.updateDisplay()
        if (usingSecondaryDisplay) {
            // The real second display's own Presentation provides the
            // bottom screen now -- the embedded one would just be a
            // redundant, confusing extra copy. Let the top screen use
            // the whole primary display instead.
            binding.surfaceDsBottom.visibility = View.GONE
        }

        binding.surfaceDsTop.holder.addCallback(TopSurfaceCallback())
        binding.surfaceDsBottom.holder.addCallback(BottomSurfaceCallback())
        binding.surfaceDsBottom.setOnTouchListener { _, event -> onBottomScreenTouch(event) }

        buildButtonOverlay()
    }

    override fun onDestroy() {
        super.onDestroy()
        stopEmulation()
        secondaryDisplayManager.releasePresentation()
        secondaryDisplayManager.releaseVD()
        NativeLibrary.sDsEmulationActivity.clear()
    }

    override fun onStop() {
        super.onStop()
        secondaryDisplayManager.releasePresentation()
    }

    override fun onRestart() {
        super.onRestart()
        secondaryDisplayManager.updateDisplay()
    }

    override fun onPause() {
        super.onPause()
        if (isRunning) {
            NativeLibrary.dsPauseEmulation()
        }
    }

    override fun onResume() {
        super.onResume()
        if (isRunning) {
            NativeLibrary.dsUnPauseEmulation()
        }
    }

    /** (x, y) in the secondary Presentation's raw SurfaceView pixel space. */
    private fun onSecondaryDisplayTouch(x: Float, y: Float, pressed: Boolean) {
        val display = secondaryDisplayManager.availableDisplays
            .firstOrNull { it.displayId == secondaryDisplayManager.currentDisplayId }
        val metrics = android.util.DisplayMetrics()
        @Suppress("DEPRECATION")
        display?.getRealMetrics(metrics)
        val width = metrics.widthPixels.takeIf { it > 0 } ?: kDsScreenWidth
        val height = metrics.heightPixels.takeIf { it > 0 } ?: kDsScreenHeight

        val dsX = (x * kDsScreenWidth / width).toInt().coerceIn(0, kDsScreenWidth - 1)
        val dsY = (y * kDsScreenHeight / height).toInt().coerceIn(0, kDsScreenHeight - 1)
        NativeLibrary.dsOnTouchEvent(dsX, dsY, pressed)
    }

    private fun startEmulationIfNeeded() {
        if (isRunning) {
            return
        }
        isRunning = true
        runThread = Thread({ NativeLibrary.dsRun(romPath) }, "DsEmulation").also {
            // melonDS's ARMJIT compiler constructor generates a large
            // number of code trampolines with heavy local register/branch
            // bookkeeping (same reasoning as DSEmuThread's setStackSize on
            // the Qt frontend) -- the platform default thread stack can be
            // too small for it.
            it.start()
        }
    }

    private fun stopEmulation() {
        if (!isRunning) {
            return
        }
        isRunning = false
        NativeLibrary.dsStopEmulation()
        runThread?.join()
        runThread = null
    }

    /** Called from [NativeLibrary.exitDsEmulationActivity] on load failure. */
    fun showLoadError(resultCode: Int) {
        AlertDialog.Builder(this)
            .setTitle("SweepDS Emu")
            .setMessage("Failed to load DS ROM (status $resultCode)")
            .setPositiveButton(android.R.string.ok) { _, _ -> finish() }
            .setCancelable(false)
            .show()
    }

    /**
     * Called from [NativeLibrary.exitDsEmulationActivity] when the
     * emulated console powers itself off -- not an error, matches real
     * DS/DSi hardware (e.g. after the firmware settings wizard finishes),
     * same as DSPlayerWindow::OnConsolePoweredOff on the Qt frontend.
     */
    fun showConsolePoweredOff() {
        AlertDialog.Builder(this)
            .setTitle("SweepDS Emu")
            .setMessage("The DS console powered itself off.")
            .setPositiveButton(android.R.string.ok) { _, _ -> finish() }
            .setCancelable(false)
            .show()
    }

    // --- Surfaces ---

    private inner class TopSurfaceCallback : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            NativeLibrary.dsTopSurfaceChanged(holder.surface)
            startEmulationIfNeeded()
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            NativeLibrary.dsTopSurfaceChanged(holder.surface)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            NativeLibrary.dsTopSurfaceDestroyed()
        }
    }

    private inner class BottomSurfaceCallback : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            NativeLibrary.dsBottomSurfaceChanged(holder.surface)
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            NativeLibrary.dsBottomSurfaceChanged(holder.surface)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            NativeLibrary.dsBottomSurfaceDestroyed()
        }
    }

    // --- Touch (stylus) input on the bottom screen ---

    private fun onBottomScreenTouch(event: MotionEvent): Boolean {
        val view = binding.surfaceDsBottom
        if (view.width <= 0 || view.height <= 0) {
            return false
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                val scaleX = kDsScreenWidth.toFloat() / view.width
                val scaleY = kDsScreenHeight.toFloat() / view.height
                val x = (event.x * scaleX).toInt().coerceIn(0, kDsScreenWidth - 1)
                val y = (event.y * scaleY).toInt().coerceIn(0, kDsScreenHeight - 1)
                NativeLibrary.dsOnTouchEvent(x, y, true)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                NativeLibrary.dsOnTouchEvent(0, 0, false)
            }
        }
        return true
    }

    // --- On-screen button overlay ---
    //
    // Reuses the exact same button artwork InputOverlay draws for the 3DS
    // side (R.drawable.button_a/_b/_x/_y/_l/_r/_start/_select/_home, each
    // with a matching "_pressed" variant) instead of the plain text-label
    // Buttons this used to be, so the DS player's overlay looks like the
    // rest of the app rather than a placeholder. Left as a fixed layout
    // (no drag-to-reposition/per-game config) rather than pulling in the
    // rest of InputOverlay's machinery, which is built entirely around
    // the 3DS's own button IDs and settings keys -- DS has few enough
    // buttons that this is a reasonable place to stop short of that.

    private fun buildButtonOverlay() {
        val overlay = binding.dsButtonOverlay

        val dpad = DsDpadView(this)
        val dpadSize = dpToPx(168)
        overlay.addView(
            dpad,
            FrameLayout.LayoutParams(dpadSize, dpadSize).apply {
                gravity = Gravity.TOP or Gravity.START
                leftMargin = dpToPx(24)
                topMargin = dpToPx(16)
            }
        )

        val faceSize = 64
        val faceGap = 8
        addOverlayButton(overlay, R.drawable.button_y, R.drawable.button_y_pressed, faceSize,
            Gravity.TOP or Gravity.END, faceSize + faceGap, 0, NativeLibrary.DsButtonType.BUTTON_Y)
        addOverlayButton(overlay, R.drawable.button_a, R.drawable.button_a_pressed, faceSize,
            Gravity.TOP or Gravity.END, 2 * faceSize + 2 * faceGap, faceSize + faceGap,
            NativeLibrary.DsButtonType.BUTTON_A)
        addOverlayButton(overlay, R.drawable.button_b, R.drawable.button_b_pressed, faceSize,
            Gravity.TOP or Gravity.END, 0, faceSize + faceGap, NativeLibrary.DsButtonType.BUTTON_B)
        addOverlayButton(overlay, R.drawable.button_x, R.drawable.button_x_pressed, faceSize,
            Gravity.TOP or Gravity.END, faceSize + faceGap, 2 * faceSize + 2 * faceGap,
            NativeLibrary.DsButtonType.BUTTON_X)

        addOverlayButton(overlay, R.drawable.button_l, R.drawable.button_l_pressed, 56,
            Gravity.TOP or Gravity.START, 16, 16, NativeLibrary.DsButtonType.BUTTON_L)
        addOverlayButton(overlay, R.drawable.button_r, R.drawable.button_r_pressed, 56,
            Gravity.TOP or Gravity.END, 16, 16, NativeLibrary.DsButtonType.BUTTON_R)

        addOverlayButton(overlay, R.drawable.button_select, R.drawable.button_select_pressed, 48,
            Gravity.BOTTOM or Gravity.START, 16, 16, NativeLibrary.DsButtonType.BUTTON_SELECT)
        addOverlayButton(overlay, R.drawable.button_start, R.drawable.button_start_pressed, 48,
            Gravity.BOTTOM or Gravity.END, 16, 16, NativeLibrary.DsButtonType.BUTTON_START)

        val homeButton = ImageButton(this).apply {
            setImageResource(R.drawable.button_home)
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            alpha = 0.85f
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> setImageResource(R.drawable.button_home_pressed)
                    MotionEvent.ACTION_UP -> {
                        setImageResource(R.drawable.button_home)
                        confirmReturnToThreeDsHomeMenu()
                    }
                    MotionEvent.ACTION_CANCEL -> setImageResource(R.drawable.button_home)
                }
                true
            }
        }
        val homeSize = dpToPx(44)
        overlay.addView(
            homeButton,
            FrameLayout.LayoutParams(homeSize, homeSize).apply {
                gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
                bottomMargin = dpToPx(16)
            }
        )
    }

    private fun addOverlayButton(
        parent: FrameLayout,
        @DrawableRes normalRes: Int,
        @DrawableRes pressedRes: Int,
        sizeDp: Int,
        gravity: Int,
        marginX: Int,
        marginY: Int,
        dsButtonBit: Int
    ) {
        val button = ImageButton(this).apply {
            setImageResource(normalRes)
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            alpha = 0.85f
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        setImageResource(pressedRes)
                        NativeLibrary.dsOnButtonEvent(dsButtonBit, true)
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        setImageResource(normalRes)
                        NativeLibrary.dsOnButtonEvent(dsButtonBit, false)
                    }
                }
                true
            }
        }
        val size = dpToPx(sizeDp)
        val params = FrameLayout.LayoutParams(size, size).apply {
            this.gravity = gravity
            leftMargin = dpToPx(marginX)
            topMargin = dpToPx(marginY)
            rightMargin = dpToPx(marginX)
            bottomMargin = dpToPx(marginY)
        }
        parent.addView(button, params)
    }

    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).toInt()

    /**
     * The HOME button (on-screen or physical) closes the current DS game
     * to return to the emulated 3DS HOME Menu -- confirm first so a
     * misplaced tap mid-game doesn't lose unsaved progress.
     */
    private fun confirmReturnToThreeDsHomeMenu() {
        AlertDialog.Builder(this)
            .setTitle("Return to HOME Menu?")
            .setMessage("This will close the current DS game.")
            .setPositiveButton(android.R.string.ok) { _, _ -> returnToThreeDsHomeMenu() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    // --- Physical keyboard / Bluetooth controller input ---

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.repeatCount > 0) {
            // No auto-repeat for DS buttons -- matches DSPlayerWindow's
            // keyPressEvent()/isAutoRepeat() check on the Qt frontend.
            return true
        }

        if (event.keyCode == kReturnToHomeMenuKeyCode) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                confirmReturnToThreeDsHomeMenu()
            }
            return true
        }

        val buttonBit = keyCodeToDsButton(event.keyCode)
        if (buttonBit != 0) {
            NativeLibrary.dsOnButtonEvent(buttonBit, event.action == KeyEvent.ACTION_DOWN)
            return true
        }

        return super.dispatchKeyEvent(event)
    }

    // Many Bluetooth gamepads report their D-pad as a HAT axis (continuous
    // motion event) rather than discrete KEYCODE_DPAD_* key events --
    // dispatchKeyEvent alone misses those. Track the last HAT-derived
    // D-pad state so we only send press/release transitions, matching the
    // discrete semantics dsOnButtonEvent expects.
    private var hatDpadState = 0

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and android.view.InputDevice.SOURCE_JOYSTICK ==
            android.view.InputDevice.SOURCE_JOYSTICK
        ) {
            val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)

            var newState = 0
            if (hatX < -0.5f) newState = newState or NativeLibrary.DsButtonType.DPAD_LEFT
            if (hatX > 0.5f) newState = newState or NativeLibrary.DsButtonType.DPAD_RIGHT
            if (hatY < -0.5f) newState = newState or NativeLibrary.DsButtonType.DPAD_UP
            if (hatY > 0.5f) newState = newState or NativeLibrary.DsButtonType.DPAD_DOWN

            val released = hatDpadState and newState.inv()
            val pressed = newState and hatDpadState.inv()
            for (bit in listOf(
                NativeLibrary.DsButtonType.DPAD_UP, NativeLibrary.DsButtonType.DPAD_DOWN,
                NativeLibrary.DsButtonType.DPAD_LEFT, NativeLibrary.DsButtonType.DPAD_RIGHT
            )) {
                if (released and bit != 0) NativeLibrary.dsOnButtonEvent(bit, false)
                if (pressed and bit != 0) NativeLibrary.dsOnButtonEvent(bit, true)
            }
            hatDpadState = newState
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    /**
     * Maps both standard Android gamepad key codes (what a Bluetooth
     * controller's HID inputs get translated into by the OS) and common
     * keyboard fallbacks onto DS buttons. Not yet user-remappable --
     * matches DSControlsConfig's *defaults* on the Qt frontend, not its
     * full rebinding UI.
     */
    private fun keyCodeToDsButton(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_Z -> NativeLibrary.DsButtonType.BUTTON_A
        KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_X -> NativeLibrary.DsButtonType.BUTTON_B
        KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_A -> NativeLibrary.DsButtonType.BUTTON_X
        KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_S -> NativeLibrary.DsButtonType.BUTTON_Y
        KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_Q -> NativeLibrary.DsButtonType.BUTTON_L
        KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_W -> NativeLibrary.DsButtonType.BUTTON_R
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER -> NativeLibrary.DsButtonType.BUTTON_START
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_SPACE -> NativeLibrary.DsButtonType.BUTTON_SELECT
        KeyEvent.KEYCODE_DPAD_UP -> NativeLibrary.DsButtonType.DPAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> NativeLibrary.DsButtonType.DPAD_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> NativeLibrary.DsButtonType.DPAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> NativeLibrary.DsButtonType.DPAD_RIGHT
        else -> 0
    }

    /**
     * Stops the current DS session and boots the 3DS HOME Menu for the
     * user's configured region -- same shape as
     * GMainWindow::BootHomeMenuForCurrentRegion on the Qt frontend, since
     * a DS forwarder's whole point is to be launched *from* and return
     * *to* the emulated 3DS HOME Menu, not the Android game list.
     */
    private fun returnToThreeDsHomeMenu() {
        stopEmulation()

        var homeMenuPath = ""
        for (region in 0 until 6) {
            val path = NativeLibrary.getHomeMenuPath(region)
            if (path.isNotEmpty()) {
                homeMenuPath = path
                break
            }
        }
        if (homeMenuPath.isEmpty()) {
            finish()
            return
        }

        val game = org.citra.citra_emu.model.Game(path = homeMenuPath, filename = "HomeMenu")
        val intent = android.content.Intent(this, EmulationActivity::class.java)
        intent.putExtra("game", game)
        startActivity(intent)
        finish()
    }

    companion object {
        const val EXTRA_DS_ROM_PATH = "DsRomPath"
        private const val kDsScreenWidth = 256
        private const val kDsScreenHeight = 192

        // F12 by default on the Qt frontend (DSControlsConfig); there's no
        // direct KeyEvent equivalent guaranteed present on every Android
        // keyboard, so BUTTON_MODE (the controller "menu"/"guide" button on
        // most Bluetooth gamepads) doubles as the same hotkey here. The
        // on-screen HOME button above covers touch-only sessions.
        private const val kReturnToHomeMenuKeyCode = KeyEvent.KEYCODE_BUTTON_MODE
    }
}
