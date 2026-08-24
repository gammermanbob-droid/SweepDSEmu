// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.activities

import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.databinding.ActivityDsEmulationBinding

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
    private var romPath: String = ""
    private var runThread: Thread? = null
    private var isRunning = false

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

        binding.surfaceDsTop.holder.addCallback(TopSurfaceCallback())
        binding.surfaceDsBottom.holder.addCallback(BottomSurfaceCallback())
        binding.surfaceDsBottom.setOnTouchListener { _, event -> onBottomScreenTouch(event) }

        buildButtonOverlay()
    }

    override fun onDestroy() {
        super.onDestroy()
        stopEmulation()
        NativeLibrary.sDsEmulationActivity.clear()
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

    private fun buildButtonOverlay() {
        val overlay = binding.dsButtonOverlay
        addOverlayButton(overlay, "▲", Gravity.TOP or Gravity.START, 220, 16, NativeLibrary.DsButtonType.DPAD_UP)
        addOverlayButton(overlay, "▼", Gravity.TOP or Gravity.START, 220, 116, NativeLibrary.DsButtonType.DPAD_DOWN)
        addOverlayButton(overlay, "◀", Gravity.TOP or Gravity.START, 160, 66, NativeLibrary.DsButtonType.DPAD_LEFT)
        addOverlayButton(overlay, "▶", Gravity.TOP or Gravity.START, 280, 66, NativeLibrary.DsButtonType.DPAD_RIGHT)

        addOverlayButton(overlay, "Y", Gravity.TOP or Gravity.END, 220, 16, NativeLibrary.DsButtonType.BUTTON_Y)
        addOverlayButton(overlay, "A", Gravity.TOP or Gravity.END, 160, 66, NativeLibrary.DsButtonType.BUTTON_A)
        addOverlayButton(overlay, "B", Gravity.TOP or Gravity.END, 280, 66, NativeLibrary.DsButtonType.BUTTON_B)
        addOverlayButton(overlay, "X", Gravity.TOP or Gravity.END, 220, 116, NativeLibrary.DsButtonType.BUTTON_X)

        addOverlayButton(overlay, "L", Gravity.TOP or Gravity.START, 16, 16, NativeLibrary.DsButtonType.BUTTON_L)
        addOverlayButton(overlay, "R", Gravity.TOP or Gravity.END, 16, 16, NativeLibrary.DsButtonType.BUTTON_R)

        addOverlayButton(overlay, "SELECT", Gravity.BOTTOM or Gravity.START, 16, 16, NativeLibrary.DsButtonType.BUTTON_SELECT)
        addOverlayButton(overlay, "START", Gravity.BOTTOM or Gravity.END, 16, 16, NativeLibrary.DsButtonType.BUTTON_START)

        val homeButton = Button(this).apply {
            text = "HOME"
            alpha = 0.6f
            setOnClickListener { returnToThreeDsHomeMenu() }
        }
        val homeParams = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            bottomMargin = 16
        }
        overlay.addView(homeButton, homeParams)
    }

    private fun addOverlayButton(
        parent: FrameLayout,
        label: String,
        gravity: Int,
        marginX: Int,
        marginY: Int,
        dsButtonBit: Int
    ) {
        val button = Button(this).apply {
            text = label
            alpha = 0.6f
            setBackgroundColor(Color.DKGRAY)
            setOnTouchListener { v, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        NativeLibrary.dsOnButtonEvent(dsButtonBit, true)
                        v.isPressed = true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        NativeLibrary.dsOnButtonEvent(dsButtonBit, false)
                        v.isPressed = false
                    }
                }
                true
            }
        }
        val params = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            this.gravity = gravity
            leftMargin = marginX
            topMargin = marginY
            rightMargin = marginX
            bottomMargin = marginY
        }
        parent.addView(button, params)
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
                returnToThreeDsHomeMenu()
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
