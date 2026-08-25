// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.activities

import android.graphics.Rect
import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.res.ResourcesCompat
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.databinding.ActivityDsEmulationBinding
import org.citra.citra_emu.display.SecondaryDisplay
import org.citra.citra_emu.features.settings.ui.SettingsActivity
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.overlay.DsButtonOverlayView
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
    private var isPaused = false
    private var overlayVisible = true

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
        setUpInGameMenu()
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
    // The face/shoulder/start-select buttons are drawn by DsButtonOverlayView,
    // a DS-specific rebuild of InputOverlay's own canvas-drawn button
    // rendering (see that class's own doc comment for why a plain
    // ImageButton, this code's previous approach, looked visibly
    // different from the 3DS side's overlay despite using the same
    // artwork) -- it also carries its own drag-to-reposition support,
    // toggled via the drawer menu's "Configure Controls" entry (see
    // showOverlayMenu/setUpInGameMenu), matching the 3DS side's own
    // "Edit Layout" option.
    private lateinit var dsButtonOverlayView: DsButtonOverlayView
    private lateinit var dsDpadView: DsDpadView

    private fun buildButtonOverlay() {
        val overlay = binding.dsButtonOverlay

        dsDpadView = DsDpadView(this)
        overlay.addView(dsDpadView, FrameLayout.LayoutParams(0, 0)) // sized/positioned by layoutDsScreens()

        dsButtonOverlayView = DsButtonOverlayView(this)
        overlay.addView(dsButtonOverlayView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))

        // Opens the in-game drawer menu (see setUpInGameMenu) -- same role
        // as the Android back button on the 3DS side (EmulationFragment
        // toggles binding.drawerLayout there), not a direct shortcut to
        // closing the game; "Exit" inside that menu covers that instead,
        // matching menu_in_game.xml's own menu_exit entry. Deliberately
        // NOT part of the housing-zone layout below -- it's a floating
        // control overlaid on the game view itself (matching how the
        // physical HOME button sits below a real 3DS's own screens),
        // not a gameplay button that needs to avoid the screens.
        val homeButton = ImageButton(this).apply {
            setImageResource(R.drawable.button_home)
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            alpha = 0.85f
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> setImageResource(R.drawable.button_home_pressed)
                    MotionEvent.ACTION_UP -> {
                        setImageResource(R.drawable.button_home)
                        toggleDrawer()
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

        setUpScreenLayout()
    }

    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).toInt()

    // --- Screen layout ---
    //
    // ds_native.cpp fixes each SurfaceView's buffer at the DS's native
    // 256x192 via ANativeWindow_setBuffersGeometry; the system
    // compositor then stretches that fixed-size buffer to whatever the
    // SurfaceView's own laid-out bounds are. This used to be a
    // LinearLayout weight=1 vertical split (match_parent width, half
    // height each), which squashed/stretched the image on any window
    // that wasn't already exactly the right proportions -- true on
    // almost every real device -- and left literally no free space
    // anywhere for the button overlay, since the screens claimed 100%
    // of both dimensions between them.
    //
    // This computes an actual 4:3 rect for each screen instead, with a
    // gap between them approximating a real DS's hinge (SCREEN_GAP_FRACTION
    // -- a deliberate visual approximation scaled to the window, not a
    // claimed precise hardware measurement) and exposes the leftover
    // width on either side as square "housing zones" the button overlay
    // (see DsButtonOverlayView.setHousingZones) defaults its buttons
    // into, so they land beside the screens instead of on top of them.
    private var lastContainerW = -1
    private var lastContainerH = -1

    private fun setUpScreenLayout() {
        binding.dsContentContainer.viewTreeObserver.addOnGlobalLayoutListener {
            val w = binding.dsContentContainer.width
            val h = binding.dsContentContainer.height
            if (w <= 0 || h <= 0 || (w == lastContainerW && h == lastContainerH)) {
                return@addOnGlobalLayoutListener
            }
            lastContainerW = w
            lastContainerH = h
            layoutDsScreens(w, h)
        }
    }

    private fun layoutDsScreens(containerW: Int, containerH: Int) {
        val gapPx = (containerH * SCREEN_GAP_FRACTION).toInt()
        // Caps each screen's width so there's always at least some
        // housing zone on each side even on an unusually wide/short
        // window -- on a typical phone in landscape this cap doesn't
        // actually kick in; height ends up the limiting dimension and
        // there's plenty of leftover width regardless.
        val maxScreenW = (containerW * MAX_SCREEN_WIDTH_FRACTION).toInt()
        val heightConstrainedW =
            ((containerH - gapPx) / 2f * kDsScreenWidth / kDsScreenHeight).toInt()
        val screenW = minOf(heightConstrainedW, maxScreenW).coerceAtLeast(1)
        val screenH = (screenW.toFloat() * kDsScreenHeight / kDsScreenWidth).toInt().coerceAtLeast(1)

        val totalContentH = 2 * screenH + gapPx
        val topInset = ((containerH - totalContentH) / 2).coerceAtLeast(0)
        val screenX = (containerW - screenW) / 2

        binding.surfaceDsTop.layoutParams = FrameLayout.LayoutParams(screenW, screenH).apply {
            leftMargin = screenX
            topMargin = topInset
        }
        binding.surfaceDsBottom.layoutParams = FrameLayout.LayoutParams(screenW, screenH).apply {
            leftMargin = screenX
            topMargin = topInset + screenH + gapPx
        }

        // "A square" per housing zone, as close as the available gutter
        // allows -- side length is whichever is smaller of the leftover
        // width and the full container height, centered vertically.
        val zoneSide = screenX.coerceAtMost(containerH)
        val zoneTop = (containerH - zoneSide) / 2
        val leftZone = Rect(0, zoneTop, zoneSide, zoneTop + zoneSide)
        val rightZone = Rect(containerW - zoneSide, zoneTop, containerW, zoneTop + zoneSide)

        val dpadSize = (zoneSide * 0.85f).toInt()
        dsDpadView.layoutParams = FrameLayout.LayoutParams(dpadSize, dpadSize).apply {
            leftMargin = leftZone.left + (leftZone.width() - dpadSize) / 2
            topMargin = leftZone.top + (leftZone.height() - dpadSize) / 2
        }

        dsButtonOverlayView.setHousingZones(leftZone, rightZone)
    }

    /**
     * "Exit" in the in-game drawer menu (see setUpInGameMenu) closes the
     * current DS game and returns to the emulated 3DS HOME Menu --
     * confirm first so a misplaced tap doesn't lose unsaved progress.
     * Same shape as EmulationFragment's own menu_exit handler.
     */
    private fun confirmReturnToThreeDsHomeMenu() {
        AlertDialog.Builder(this)
            .setTitle("Return to HOME Menu?")
            .setMessage("This will close the current DS game.")
            .setPositiveButton(android.R.string.ok) { _, _ -> returnToThreeDsHomeMenu() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    // --- In-game drawer menu ---
    //
    // Same DrawerLayout + NavigationView shape as EmulationFragment's own
    // in-game menu (see activity_ds_emulation.xml / menu_ds_in_game.xml),
    // trimmed to what actually applies to a DS session -- no Amiibo,
    // screen-layout switching, Cheats, or Multiplayer entries, since this
    // core doesn't support any of those.

    private fun toggleDrawer() {
        if (binding.drawerLayout.isOpen) {
            binding.drawerLayout.close()
        } else {
            binding.drawerLayout.open()
        }
    }

    private fun setUpInGameMenu() {
        binding.inGameMenu.getHeaderView(0).apply {
            findViewById<TextView>(R.id.text_game_title).text = gameTitleFromRomPath()
        }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = toggleDrawer()
        })

        binding.inGameMenu.setNavigationItemSelectedListener {
            when (it.itemId) {
                R.id.menu_emulation_pause -> {
                    if (isPaused) {
                        NativeLibrary.dsUnPauseEmulation()
                        it.title = getString(R.string.pause_emulation)
                        it.icon = ResourcesCompat.getDrawable(resources, R.drawable.ic_pause, theme)
                    } else {
                        NativeLibrary.dsPauseEmulation()
                        it.title = getString(R.string.resume_emulation)
                        it.icon = ResourcesCompat.getDrawable(resources, R.drawable.ic_play, theme)
                    }
                    isPaused = !isPaused
                    true
                }
                R.id.menu_emulation_savestates -> {
                    showSaveStateMenu()
                    true
                }
                R.id.menu_overlay_options -> {
                    showOverlayMenu()
                    true
                }
                R.id.menu_settings -> {
                    SettingsActivity.launch(this, SettingsFile.FILE_NAME_CONFIG, "")
                    true
                }
                R.id.menu_exit -> {
                    binding.drawerLayout.close()
                    confirmReturnToThreeDsHomeMenu()
                    true
                }
                else -> true
            }
        }
    }

    private fun gameTitleFromRomPath(): String {
        val base = romPath.substringAfterLast('/')
        return base.substringBeforeLast('.').ifEmpty { base }
    }

    private fun showOverlayMenu() {
        val popupMenu = PopupMenu(this, binding.inGameMenu.findViewById(R.id.menu_overlay_options))
        popupMenu.menuInflater.inflate(R.menu.menu_ds_overlay_options, popupMenu.menu)
        popupMenu.menu.findItem(R.id.menu_show_overlay).isChecked = overlayVisible
        popupMenu.menu.findItem(R.id.menu_emulation_edit_layout).isChecked =
            dsButtonOverlayView.repositionModeEnabled
        popupMenu.setOnMenuItemClickListener {
            when (it.itemId) {
                R.id.menu_show_overlay -> {
                    overlayVisible = !overlayVisible
                    binding.dsButtonOverlay.visibility = if (overlayVisible) View.VISIBLE else View.GONE
                }
                R.id.menu_emulation_edit_layout -> {
                    dsButtonOverlayView.repositionModeEnabled = !dsButtonOverlayView.repositionModeEnabled
                    Toast.makeText(
                        this,
                        if (dsButtonOverlayView.repositionModeEnabled)
                            "Drag buttons to reposition them" else "Layout saved",
                        Toast.LENGTH_SHORT
                    ).show()
                }
                R.id.menu_emulation_reset_overlay -> {
                    dsButtonOverlayView.resetPositions()
                }
            }
            true
        }
        popupMenu.show()
    }

    // Save states live in this app's own real storage (not any of
    // Azahar's "sdmc:/..." virtual paths -- MelonDSCore::SaveState/
    // LoadState take the path given verbatim, no translation, so this
    // has to already be a genuine filesystem path), one subfolder per
    // ROM so two games' slots can never collide.
    private fun saveStateDir(): File {
        val dir = File(filesDir, "ds_savestates/" + gameTitleFromRomPath())
        dir.mkdirs()
        return dir
    }

    private fun showSaveStateMenu() {
        val popupMenu = PopupMenu(this, binding.inGameMenu.findViewById(R.id.menu_emulation_savestates))
        popupMenu.menuInflater.inflate(R.menu.menu_savestates, popupMenu.menu)
        popupMenu.setOnMenuItemClickListener {
            when (it.itemId) {
                R.id.menu_emulation_save_state -> showStateSlotMenu(isSaving = true)
                R.id.menu_emulation_load_state -> showStateSlotMenu(isSaving = false)
            }
            true
        }
        popupMenu.show()
    }

    private fun showStateSlotMenu(isSaving: Boolean) {
        val dateFormat = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
        val popupMenu = PopupMenu(this, binding.inGameMenu.findViewById(R.id.menu_emulation_savestates))
        for (slot in 1..kSaveStateSlotCount) {
            val file = File(saveStateDir(), "slot$slot.state")
            val exists = file.exists()
            val label = if (exists) {
                "Slot $slot -- ${dateFormat.format(Date(file.lastModified()))}"
            } else {
                "Slot $slot -- empty"
            }
            popupMenu.menu.add(label).apply {
                isEnabled = isSaving || exists
                setOnMenuItemClickListener {
                    if (isSaving) {
                        NativeLibrary.dsSaveState(file.absolutePath)
                        Toast.makeText(this@DsEmulationActivity, "Saving...", Toast.LENGTH_SHORT).show()
                    } else {
                        NativeLibrary.dsLoadState(file.absolutePath)
                        binding.drawerLayout.close()
                        Toast.makeText(this@DsEmulationActivity, "Loading...", Toast.LENGTH_SHORT).show()
                    }
                    true
                }
            }
        }
        popupMenu.show()
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
                toggleDrawer()
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
        private const val kSaveStateSlotCount = 4

        // See layoutDsScreens()'s own doc comment -- deliberate visual
        // approximations of a real DS's hinge gap and a sensible
        // maximum screen width, not precise hardware measurements.
        private const val SCREEN_GAP_FRACTION = 0.05f
        private const val MAX_SCREEN_WIDTH_FRACTION = 0.72f

        // F12 by default on the Qt frontend (DSControlsConfig); there's no
        // direct KeyEvent equivalent guaranteed present on every Android
        // keyboard, so BUTTON_MODE (the controller "menu"/"guide" button on
        // most Bluetooth gamepads) doubles as the same hotkey here. The
        // on-screen HOME button above covers touch-only sessions.
        private const val kReturnToHomeMenuKeyCode = KeyEvent.KEYCODE_BUTTON_MODE
    }
}
