// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.activities

import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.os.Process
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
import org.citra.citra_emu.features.settings.model.Settings
import org.citra.citra_emu.features.settings.model.view.InputBindingSetting
import org.citra.citra_emu.features.settings.ui.SettingsActivity
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.overlay.DsButtonOverlayView
import org.citra.citra_emu.overlay.DsDpadView
import org.citra.citra_emu.services.EmulationForegroundService
import org.citra.citra_emu.utils.ControllerMappingHelper

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

    // Set by layoutDsScreens() whenever !usingSecondaryDisplay -- the
    // bottom screen's own top-edge offset and height within the single
    // combined surfaceDsTop surface, in that view's own pixel space, so
    // onCombinedScreenTouch() can tell a tap on the bottom (touch) screen
    // apart from one landing on the top screen or the gap between them.
    private var combinedBottomOffsetPx = 0
    private var combinedScreenHeightPx = 1

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

        // Empty is a legitimate value here, not a caller mistake: it's
        // MelonDSCore::Load's own convention for "boot straight to the DSi
        // Menu, no cart" (see its boot_to_menu and HomeSettingsFragment's
        // "Boot DSi Menu" entry, the only other place that launches this
        // Activity with EXTRA_DS_ROM_PATH deliberately left unset).
        romPath = intent.getStringExtra(EXTRA_DS_ROM_PATH) ?: ""

        NativeLibrary.sDsEmulationActivity = java.lang.ref.WeakReference(this)

        secondaryDisplayManager = SecondaryDisplay(
            this,
            onSurfaceUpdate = { NativeLibrary.dsBottomSurfaceChanged(it) },
            onSurfaceDestroy = { NativeLibrary.dsBottomSurfaceDestroyed() },
            onTouch = { x, y, pressed -> onSecondaryDisplayTouch(x, y, pressed) },
            onTouchMoved = { x, y -> onSecondaryDisplayTouch(x, y, true) }
        )
        secondaryDisplayManager.updateDisplay()
        binding.surfaceDsTop.holder.addCallback(TopSurfaceCallback())

        if (usingSecondaryDisplay) {
            // A genuine second physical display (Thor/Odin-style hardware)
            // already provides the bottom screen through its own
            // Presentation surface -- the embedded surfaceDsBottom would
            // just be a redundant, confusing extra copy. Let the top
            // screen use the primary display on its own. Both screens go
            // to genuinely separate physical displays/windows here, so
            // there's no same-window z-order ambiguity between them to
            // resolve either way -- setZOrderMediaOverlay kept purely for
            // parity with how this path behaved before the single-surface
            // fix below existed, not because it's known to still matter.
            binding.surfaceDsTop.setZOrderMediaOverlay(true)
            binding.surfaceDsBottom.visibility = View.GONE
            binding.surfaceDsBottom.setZOrderMediaOverlay(true)
            binding.surfaceDsBottom.holder.addCallback(BottomSurfaceCallback())
            binding.surfaceDsBottom.setOnTouchListener { _, event -> onBottomScreenTouch(event) }
        } else {
            // Normal single-display phones/tablets: both DS screens are
            // blitted into surfaceDsTop alone (top screen, then bottom,
            // stacked with a gap -- see layoutDsScreens/dsSetScreenGap)
            // instead of using two separate SurfaceViews. Two co-existing
            // SurfaceViews in one window is a known Android compositor
            // trigger for one of them getting stuck on its last frame
            // after the Activity is covered and uncovered again (e.g. by
            // Settings) -- confirmed via native-side logging that fresh
            // buffers kept getting posted successfully to the "frozen"
            // surface the whole time, meaning the app side was already
            // doing everything right and no combination of Z-ordering,
            // view reattachment, or window translucency fixed it. A
            // single surface has no second layer for the compositor to
            // lose track of, so unlike the branch above this deliberately
            // does NOT call setZOrderMediaOverlay -- that flag promotes
            // the surface's content above every regular View in this
            // window (including ds_loading_overlay), which silently hid
            // the loading screen behind the DS surface's own blank
            // initial buffer. SurfaceView's real default (content behind
            // the window, normal Views composite in front of it) is
            // exactly what's wanted here now that there's only one.
            // surfaceDsBottom stays unused/hidden here.
            binding.surfaceDsBottom.visibility = View.GONE
            binding.surfaceDsTop.setOnTouchListener { _, event -> onCombinedScreenTouch(event) }
        }

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
        // Picks up a DS Screen Size change made from the Settings screen
        // while this Activity was backgrounded -- the container's own
        // dimensions haven't changed, so the layout listener in
        // setUpScreenLayout() wouldn't otherwise re-run layoutDsScreens().
        val w = binding.dsContentContainer.width
        val h = binding.dsContentContainer.height
        if (w > 0 && h > 0) {
            layoutDsScreens(w, h)
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
        startForegroundService(
            Intent(this, EmulationForegroundService::class.java).apply {
                putExtra(EmulationForegroundService.EXTRA_TITLE, gameTitleFromRomPath())
                putExtra(
                    EmulationForegroundService.EXTRA_REOPEN_INTENT,
                    Intent(this@DsEmulationActivity, DsEmulationActivity::class.java).apply {
                        putExtra(EXTRA_DS_ROM_PATH, romPath)
                    }
                )
            }
        )
        runThread = Thread({
            // Some OEM power managers (e.g. Samsung's background process
            // throttling) can drop a backgrounded app's threads to a lower
            // CPU scheduling class, which can desync cycle-accurate emulator
            // timing even though the thread keeps getting scheduled at all.
            // THREAD_PRIORITY_URGENT_AUDIO is the standard real-time class
            // media/audio engines use to stay exempt from that throttling.
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            NativeLibrary.dsRun(romPath)
        }, "DsEmulation").also {
            // melonDS's ARMJIT compiler constructor generates a large
            // number of code trampolines with heavy local register/branch
            // bookkeeping (same reasoning as DSEmuThread's setStackSize on
            // the Qt frontend) -- the platform default thread stack can be
            // too small for it.
            it.start()
        }
        if (preferences.getBoolean(Settings.KEY_DS_AUTO_SAVESTATE, true)) {
            val file = autoSaveStateFile()
            if (file.exists()) {
                NativeLibrary.dsLoadState(file.absolutePath)
            }
        }
    }

    private fun stopEmulation() {
        if (!isRunning) {
            return
        }
        isRunning = false
        val autoSavePath = if (preferences.getBoolean(Settings.KEY_DS_AUTO_SAVESTATE, true)) {
            autoSaveStateFile().absolutePath
        } else {
            ""
        }
        NativeLibrary.dsStopEmulation(autoSavePath)
        runThread?.join()
        runThread = null
        stopService(Intent(this, EmulationForegroundService::class.java))
    }

    /** Called from [NativeLibrary.notifyDsFirstFrame] once real output exists to show. */
    fun hideLoadingScreen() {
        binding.dsLoadingOverlay.visibility = View.GONE
    }

    /** Called from [NativeLibrary.exitDsEmulationActivity] on load failure. */
    fun showLoadError(resultCode: Int) {
        // An empty romPath only ever means "boot the DSi Menu directly"
        // (see MelonDSCore::Load's boot_to_menu) -- its only failure mode
        // is missing real DSi system files, so give that specific reason
        // instead of a generic "failed to load DS ROM" that doesn't apply
        // (there was never a ROM here to fail loading in the first place).
        val message = if (romPath.isEmpty()) {
            "Booting the DSi Menu requires real DSi system files " +
                "(bios9.bin, bios7.bin, firmware.bin, dsi_bios9.bin, dsi_bios7.bin, " +
                "dsi_firmware.bin, dsi_nand.bin in sdmc/bios/) -- see STARTUP_GUIDE.txt."
        } else {
            "Failed to load DS ROM (status $resultCode)"
        }
        AlertDialog.Builder(this)
            .setTitle("SweepDS Emu")
            .setMessage(message)
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

    /**
     * Used instead of [onBottomScreenTouch] on the single-display (non-
     * Thor/Odin) layout, where both DS screens share surfaceDsTop as one
     * combined surface -- see onCreate's doc comment. Real DS hardware
     * only accepts touch on the bottom screen, so taps landing in the top
     * screen or the gap between them are ignored.
     */
    private fun onCombinedScreenTouch(event: MotionEvent): Boolean {
        val view = binding.surfaceDsTop
        if (view.width <= 0 || view.height <= 0) {
            return false
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                if (event.y < combinedBottomOffsetPx) {
                    return false
                }
                val scaleX = kDsScreenWidth.toFloat() / view.width
                val scaleY = kDsScreenHeight.toFloat() / combinedScreenHeightPx
                val x = (event.x * scaleX).toInt().coerceIn(0, kDsScreenWidth - 1)
                val y = ((event.y - combinedBottomOffsetPx) * scaleY).toInt()
                    .coerceIn(0, kDsScreenHeight - 1)
                NativeLibrary.dsOnTouchEvent(x, y, true)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                NativeLibrary.dsOnTouchEvent(0, 0, false)
            }
        }
        return true
    }

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
    // claimed precise hardware measurement), full container width, and
    // exposes the strip of leftover height below both screens as two
    // side-by-side "housing zones" the button overlay (see
    // DsButtonOverlayView.setHousingZones) defaults its buttons into --
    // matching the official melonDS Android app's own default layout
    // rather than the side-by-side-with-the-screens arrangement this used
    // to have, which gave up screen width permanently to make room for
    // buttons even on windows with plenty of spare height to put them in
    // below instead.
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
        // Matches the official melonDS Android app's own default layout
        // (confirmed by direct pixel measurement against its APK on this
        // same device): both DS screens run the *full* container width,
        // stacked vertically, with all on-screen controls living in
        // whatever strip is left below them -- not beside the screens in
        // dedicated side columns like this activity used to do. That
        // older side-by-side layout gave up screen width permanently to
        // house the D-pad/buttons even when the window had plenty of
        // spare height to put them in instead.
        val reservedControlsH = (containerH * CONTROLS_STRIP_HEIGHT_FRACTION).toInt()
        val fullWidthScreenH =
            (containerW.toFloat() * kDsScreenHeight / kDsScreenWidth).toInt()
        val heightConstrainedScreenH = ((containerH - reservedControlsH - gapPx) / 2f).toInt()
        // User-configurable shrink (Settings.KEY_DS_SCREEN_SCALE, 50-100%,
        // default 100) on top of the auto-fit size above -- that size is
        // already the largest that fits the window while leaving the
        // controls strip its minimum height, so this only ever makes the
        // screens smaller (and the controls strip correspondingly
        // taller), never overflows the container.
        val scalePercent = preferences.getInt(Settings.KEY_DS_SCREEN_SCALE, 100).coerceIn(50, 100)
        val screenH = (minOf(fullWidthScreenH, heightConstrainedScreenH) * scalePercent / 100)
            .coerceAtLeast(1)
        val screenW = (screenH.toFloat() * kDsScreenWidth / kDsScreenHeight).toInt().coerceAtLeast(1)

        val totalContentH = 2 * screenH + gapPx
        val topInset = 0
        val screenX = (containerW - screenW) / 2

        if (usingSecondaryDisplay) {
            binding.surfaceDsTop.layoutParams = FrameLayout.LayoutParams(screenW, screenH).apply {
                leftMargin = screenX
                topMargin = topInset
            }
        } else {
            // Both screens live in one surface here (see onCreate's doc
            // comment on why) -- size it to the union of both screens plus
            // the gap between them, and tell native code where that gap
            // falls in its own buffer's pixel space so it can blit the top
            // screen, a black gap, then the bottom screen into one frame.
            binding.surfaceDsTop.layoutParams =
                FrameLayout.LayoutParams(screenW, totalContentH).apply {
                    leftMargin = screenX
                    topMargin = topInset
                }
            val gapBufferPx = Math.round(gapPx * kDsScreenWidth / screenW.toFloat())
            NativeLibrary.dsSetScreenGap(gapBufferPx)
            combinedBottomOffsetPx = screenH + gapPx
            combinedScreenHeightPx = screenH
        }

        // The whole strip below both screens, split into a left half for
        // the D-pad and a right half for the face-button diamond --
        // matches melonDS's own default arrangement (both halves are
        // wider than tall on most phones, so DsButtonOverlayView's own
        // zone.width().coerceAtMost(zone.height()) sizing naturally keys
        // off the height here without any special-casing).
        val controlsTop = totalContentH
        val leftZone = Rect(0, controlsTop, containerW / 2, containerH)
        val rightZone = Rect(containerW / 2, controlsTop, containerW, containerH)

        val dpadSize = (leftZone.height().coerceAtMost(leftZone.width()) * 0.65f).toInt()
        dsDpadView.layoutParams = FrameLayout.LayoutParams(dpadSize, dpadSize).apply {
            leftMargin = leftZone.left + (leftZone.width() - dpadSize) / 2
            topMargin = leftZone.top + (leftZone.height() - dpadSize) / 2
        }

        dsButtonOverlayView.setHousingZones(leftZone, rightZone)
    }

    /**
     * "Exit" in the in-game drawer menu, and the bound "return to HOME
     * menu" hotkey, both end here now instead of silently always
     * booting the 3DS HOME Menu: a DS game can be reached either
     * through a forwarder (where HOME Menu is the natural place to land
     * back on, matching real hardware) or picked directly from Azahar's
     * own game list (where landing back on the emulated 3DS HOME Menu
     * instead of the list the game was launched from is surprising).
     * Ask every time rather than guessing which case this is.
     */
    private fun confirmExitDestination() {
        AlertDialog.Builder(this)
            .setTitle(R.string.ds_exit_destination_title)
            .setMessage(R.string.ds_exit_destination_message)
            .setPositiveButton(R.string.ds_exit_destination_home_menu) { _, _ -> returnToThreeDsHomeMenu() }
            .setNegativeButton(R.string.ds_exit_destination_game_list) { _, _ -> goToGameList() }
            .show()
    }

    /**
     * The "no" answer to [confirmExitDestination] -- lands back on
     * Azahar's own game list (MainActivity) instead of the emulated 3DS
     * HOME Menu, clearing this and anything else DS/3DS-emulation-
     * related off the back stack so it matches what freshly opening the
     * app shows, the same way [returnToThreeDsHomeMenu] clears down to
     * a fresh HOME Menu launch for its own answer.
     */
    private fun goToGameList() {
        stopEmulation()
        val intent = Intent(this, org.citra.citra_emu.ui.main.MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
        }
        startActivity(intent)
        finish()
    }

    /**
     * "Reset Game" in the in-game drawer menu -- a DS-side equivalent of
     * a real console's power/reset button, wired to the core's existing
     * (previously unused on Android) dsRequestReset(). Useful for things
     * like Pokemon shiny-hunting, where restarting from power-on
     * repeatedly for fresh random encounters is the whole point, rather
     * than reloading a save. Confirm first since it discards unsaved
     * progress, same shape as confirmExitDestination.
     */
    private fun confirmResetGame() {
        AlertDialog.Builder(this)
            .setTitle(R.string.ds_reset_confirm_title)
            .setMessage(R.string.ds_reset_confirm_message)
            .setPositiveButton(android.R.string.ok) { _, _ -> NativeLibrary.dsRequestReset() }
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
                R.id.menu_ds_reset -> {
                    binding.drawerLayout.close()
                    confirmResetGame()
                    true
                }
                R.id.menu_ds_controls -> {
                    SettingsActivity.launch(this, Settings.SECTION_DS_CONTROLS, "")
                    true
                }
                R.id.menu_settings -> {
                    SettingsActivity.launch(this, SettingsFile.FILE_NAME_CONFIG, "")
                    true
                }
                R.id.menu_exit -> {
                    binding.drawerLayout.close()
                    confirmExitDestination()
                    true
                }
                else -> true
            }
        }
    }

    private fun gameTitleFromRomPath(): String {
        // Empty romPath means "boot straight to the DSi Menu, no cart" --
        // see MelonDSCore::Load's boot_to_menu.
        if (romPath.isEmpty()) {
            return "DSi Menu"
        }
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

    private fun autoSaveStateFile(): File = File(saveStateDir(), "auto.state")

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

        // User-configurable (DsControlsSettings) -- BUTTON_MODE is only
        // the fallback default for as long as this hotkey is unbound.
        // Used to fire returnToThreeDsHomeMenu() directly with no
        // prompt at all, on the theory that a deliberately-bound hotkey
        // press is unambiguous -- but the destination is genuinely
        // ambiguous regardless of how deliberate the press was (forwarder
        // vs. direct game-list launch, see confirmExitDestination), so
        // this now asks the same question the drawer's "Exit" does.
        if (event.keyCode == returnToHomeMenuKeyCode()) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                confirmExitDestination()
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

    private val preferences: android.content.SharedPreferences by lazy {
        androidx.preference.PreferenceManager.getDefaultSharedPreferences(this)
    }

    private fun returnToHomeMenuKeyCode(): Int {
        val bound = preferences.getInt(Settings.KEY_DS_HOTKEY_RETURN_HOME, 0)
        return if (bound != 0) bound else kReturnToHomeMenuKeyCode
    }

    // Many Bluetooth gamepads report their D-pad as a HAT axis (continuous
    // motion event) rather than discrete KEYCODE_DPAD_* key events --
    // dispatchKeyEvent alone misses those. Track the last HAT-derived
    // D-pad state so we only send press/release transitions, matching the
    // discrete semantics dsOnButtonEvent expects.
    private var hatDpadState = 0

    // Most gamepads report L2/R2 as continuous analog axes (AXIS_LTRIGGER/
    // AXIS_RTRIGGER, or AXIS_BRAKE/AXIS_GAS on some controller mappings)
    // rather than discrete KEYCODE_BUTTON_L2/R2 key events -- unlike L1/R1
    // (the bumpers), which reliably arrive as key events. dispatchKeyEvent
    // alone misses these entirely, which is why defaultKeyCodesFor's
    // KEYCODE_BUTTON_L2/R2 default for DS L/R never fired on most physical
    // controllers. Synthesize a press/release through the same
    // keyCodeToDsButton lookup used for real key events (as if
    // KEYCODE_BUTTON_L2/R2 itself were pressed) so rebinding still behaves
    // consistently, tracking last-known state the same way hatDpadState
    // does for the D-pad.
    private var triggerButtonState = 0

    /**
     * Settings.KEY_DS_DPAD_FOLLOWS_CIRCLE_PAD: scans this event's axes the
     * same way EmulationActivity.dispatchGenericMotionEvent does for the
     * 3DS Circle Pad (NativeLibrary.ButtonType.STICK_LEFT), reading
     * whatever axis/orientation/inversion the user has that bound to
     * there, and converts its current direction into a DS D-pad bitmask
     * instead. Lets a player who already rebound their Circle Pad to a
     * less-cramped stick use that same stick for the DS D-pad without a
     * separate rebind, since the DS side has no analog stick of its own
     * to bind that physical input to otherwise.
     */
    private fun circlePadAsDpadState(event: MotionEvent): Int {
        val input = event.device ?: return 0
        var x = 0f
        var y = 0f
        for (range in input.motionRanges) {
            val axis = range.axis
            val mapping = preferences.getInt(InputBindingSetting.getInputAxisButtonKey(axis), -1)
            if (mapping != NativeLibrary.ButtonType.STICK_LEFT) {
                continue
            }
            val orientation = preferences.getInt(InputBindingSetting.getInputAxisOrientationKey(axis), -1)
            if (orientation != 0 && orientation != 1) {
                continue
            }
            var value = ControllerMappingHelper.scaleAxis(input, axis, event.getAxisValue(axis))
            if (preferences.getBoolean(InputBindingSetting.getInputAxisInvertedKey(axis), false)) {
                value = -value
            }
            if (orientation == 0) x = value else y = value
        }

        var state = 0
        if (x < -0.5f) state = state or NativeLibrary.DsButtonType.DPAD_LEFT
        if (x > 0.5f) state = state or NativeLibrary.DsButtonType.DPAD_RIGHT
        if (y < -0.5f) state = state or NativeLibrary.DsButtonType.DPAD_UP
        if (y > 0.5f) state = state or NativeLibrary.DsButtonType.DPAD_DOWN
        return state
    }

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

            if (preferences.getBoolean(Settings.KEY_DS_DPAD_FOLLOWS_CIRCLE_PAD, false)) {
                newState = newState or circlePadAsDpadState(event)
            }

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

            val lTrigger = event.getAxisValue(MotionEvent.AXIS_LTRIGGER)
                .takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_BRAKE)
            val rTrigger = event.getAxisValue(MotionEvent.AXIS_RTRIGGER)
                .takeIf { it != 0f } ?: event.getAxisValue(MotionEvent.AXIS_GAS)

            var newTriggerState = 0
            val lBit = keyCodeToDsButton(KeyEvent.KEYCODE_BUTTON_L2)
            val rBit = keyCodeToDsButton(KeyEvent.KEYCODE_BUTTON_R2)
            if (lTrigger > 0.5f) newTriggerState = newTriggerState or lBit
            if (rTrigger > 0.5f) newTriggerState = newTriggerState or rBit

            val triggerReleased = triggerButtonState and newTriggerState.inv()
            val triggerPressed = newTriggerState and triggerButtonState.inv()
            for (bit in listOf(lBit, rBit)) {
                if (bit == 0) continue
                if (triggerReleased and bit != 0) NativeLibrary.dsOnButtonEvent(bit, false)
                if (triggerPressed and bit != 0) NativeLibrary.dsOnButtonEvent(bit, true)
            }
            triggerButtonState = newTriggerState

            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    /**
     * User-remappable (see DsControlsSettings/DsInputBindingSetting):
     * for each DS button, a configured binding is checked first; a
     * button that's never been rebound falls back to
     * [defaultKeyCodesFor]'s original hardcoded defaults (standard
     * Android gamepad key codes plus a keyboard fallback), so existing
     * setups keep working unchanged until the user opens the new "DS
     * Controls" settings screen.
     */
    private fun keyCodeToDsButton(keyCode: Int): Int {
        Settings.dsButtonKeys.forEachIndexed { i, key ->
            if (matchesKeyCode(key, keyCode)) {
                return remapFaceButton(dsButtonTypes[i])
            }
        }
        Settings.dsDpadKeys.forEachIndexed { i, key ->
            if (matchesKeyCode(key, keyCode)) {
                return dsDpadButtonTypes[i]
            }
        }
        return 0
    }

    /**
     * Applies the Settings.KEY_DS_SWAP_AB / KEY_DS_SWAP_XY face-button
     * swaps (for controllers with a PlayStation-style button layout) --
     * shared by both the physical key/gamepad path above and
     * DsButtonOverlayView's on-screen touch buttons, so the two stay
     * consistent regardless of input source. A no-op for any bit that
     * isn't A/B/X/Y (L/R, D-pad, Start/Select all pass through unchanged).
     */
    private fun remapFaceButton(bit: Int): Int {
        if (preferences.getBoolean(Settings.KEY_DS_SWAP_AB, false)) {
            when (bit) {
                NativeLibrary.DsButtonType.BUTTON_A -> return NativeLibrary.DsButtonType.BUTTON_B
                NativeLibrary.DsButtonType.BUTTON_B -> return NativeLibrary.DsButtonType.BUTTON_A
            }
        }
        if (preferences.getBoolean(Settings.KEY_DS_SWAP_XY, false)) {
            when (bit) {
                NativeLibrary.DsButtonType.BUTTON_X -> return NativeLibrary.DsButtonType.BUTTON_Y
                NativeLibrary.DsButtonType.BUTTON_Y -> return NativeLibrary.DsButtonType.BUTTON_X
            }
        }
        return bit
    }

    /**
     * True if [keyCode] should trigger [dsSettingKey]'s DS button: an
     * explicit user binding (if any) is the ONLY thing checked once one
     * exists -- rebinding fully replaces the default rather than
     * layering on top of it, matching the Qt frontend's own
     * DSControlsConfig rebinding UI. A button that's never been
     * rebound falls back to [defaultKeyCodesFor]'s original hardcoded
     * candidates instead, so existing setups keep working unchanged
     * until the user opens the new "DS Controls" settings screen.
     */
    private fun matchesKeyCode(dsSettingKey: String, keyCode: Int): Boolean {
        val bound = preferences.getInt(dsSettingKey, 0)
        return if (bound != 0) keyCode == bound else keyCode in defaultKeyCodesFor(dsSettingKey)
    }

    private fun defaultKeyCodesFor(dsSettingKey: String): List<Int> = when (dsSettingKey) {
        Settings.KEY_DS_BUTTON_A -> listOf(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_Z)
        Settings.KEY_DS_BUTTON_B -> listOf(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_X)
        Settings.KEY_DS_BUTTON_X -> listOf(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_A)
        Settings.KEY_DS_BUTTON_Y -> listOf(KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_S)
        // DS hardware only has one shoulder-button pair, which every
        // physical gamepad's L1/R1 bumpers map to -- matching what the
        // 3DS side's own default L/R binding uses (see
        // InputBindingSetting's KEYCODE_BUTTON_L1/R1 -> TRIGGER_L/R).
        // L2/R2 (the analog triggers) are ALSO listed since some
        // controllers only expose analog triggers as key events, but
        // L1/R1 must come first here -- these were previously L2/R2-only,
        // which is why a gamepad's real L/R bumpers (recognized fine on
        // the 3DS side) never registered as DS L/R at all.
        Settings.KEY_DS_BUTTON_L ->
            listOf(KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_Q)
        Settings.KEY_DS_BUTTON_R ->
            listOf(KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_W)
        Settings.KEY_DS_BUTTON_START -> listOf(KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER)
        Settings.KEY_DS_BUTTON_SELECT -> listOf(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_SPACE)
        Settings.KEY_DS_BUTTON_UP -> listOf(KeyEvent.KEYCODE_DPAD_UP)
        Settings.KEY_DS_BUTTON_DOWN -> listOf(KeyEvent.KEYCODE_DPAD_DOWN)
        Settings.KEY_DS_BUTTON_LEFT -> listOf(KeyEvent.KEYCODE_DPAD_LEFT)
        Settings.KEY_DS_BUTTON_RIGHT -> listOf(KeyEvent.KEYCODE_DPAD_RIGHT)
        else -> emptyList()
    }

    private val dsButtonTypes = listOf(
        NativeLibrary.DsButtonType.BUTTON_A,
        NativeLibrary.DsButtonType.BUTTON_B,
        NativeLibrary.DsButtonType.BUTTON_X,
        NativeLibrary.DsButtonType.BUTTON_Y,
        NativeLibrary.DsButtonType.BUTTON_L,
        NativeLibrary.DsButtonType.BUTTON_R,
        NativeLibrary.DsButtonType.BUTTON_SELECT,
        NativeLibrary.DsButtonType.BUTTON_START
    )
    private val dsDpadButtonTypes = listOf(
        NativeLibrary.DsButtonType.DPAD_UP,
        NativeLibrary.DsButtonType.DPAD_DOWN,
        NativeLibrary.DsButtonType.DPAD_LEFT,
        NativeLibrary.DsButtonType.DPAD_RIGHT
    )

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
        // approximation of a real DS's hinge gap, not a precise hardware
        // measurement.
        private const val SCREEN_GAP_FRACTION = 0.05f
        // Minimum height reserved for the controls strip below both
        // screens, as a fraction of the container's own height -- both
        // screens shrink (see layoutDsScreens) rather than ever eating
        // into this. ~0.28 matches the official melonDS Android app's own
        // default layout, measured directly against its APK running on
        // this same device.
        private const val CONTROLS_STRIP_HEIGHT_FRACTION = 0.28f

        // F12 by default on the Qt frontend (DSControlsConfig); there's no
        // direct KeyEvent equivalent guaranteed present on every Android
        // keyboard, so BUTTON_MODE (the controller "menu"/"guide" button on
        // most Bluetooth gamepads) doubles as the same hotkey here. The
        // on-screen HOME button above covers touch-only sessions.
        private const val kReturnToHomeMenuKeyCode = KeyEvent.KEYCODE_BUTTON_MODE
    }
}
