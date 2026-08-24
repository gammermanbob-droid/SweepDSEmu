// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.display

import android.app.Presentation
import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Build
import android.os.Bundle
import android.view.Display
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.model.IntSetting
import org.citra.citra_emu.utils.Log

/**
 * Presents a SurfaceView on a genuine second physical display when one
 * exists (dual-screen handhelds like the Odin 2 or AYN Thor -- the DS's
 * own two-screen layout maps onto this kind of hardware directly, one
 * real display per DS screen, rather than splitting a single display in
 * two). [onSurfaceUpdate]/[onSurfaceDestroy] default to the 3DS bottom
 * screen's JNI calls so [org.citra.citra_emu.activities.EmulationActivity]'s
 * existing usage is unaffected; [org.citra.citra_emu.activities.DsEmulationActivity]
 * passes the DS-specific ones instead.
 */
class SecondaryDisplay(
    val context: Context,
    private val onSurfaceUpdate: (android.view.Surface) -> Unit = { NativeLibrary.secondarySurfaceChanged(it) },
    private val onSurfaceDestroy: () -> Unit = { NativeLibrary.secondarySurfaceDestroyed() },
    // (x, y, pressed) in raw SurfaceView pixel space -- callers that need
    // different coordinate scaling (e.g. DS's 256x192 core-space, versus
    // the 3DS default below which passes raw pixels straight through to
    // NativeLibrary and lets the core scale them) do that themselves.
    private val onTouch: (Float, Float, Boolean) -> Unit =
        { x, y, pressed -> NativeLibrary.onSecondaryTouchEvent(x, y, pressed) },
    private val onTouchMoved: (Float, Float) -> Unit =
        { x, y -> NativeLibrary.onSecondaryTouchMoved(x, y) }
) : DisplayManager.DisplayListener {
    private var pres: SecondaryDisplayPresentation? = null
    private val displayManager = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val vd: VirtualDisplay
    var preferredDisplayId = -1
    var currentDisplayId = -1

    val availableDisplays: List<Display>
        get() = getSecondaryDisplays()

    init {
        vd = displayManager.createVirtualDisplay(
            "HiddenDisplay",
            1920,
            1080,
            320,
            null,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
        )
        displayManager.registerDisplayListener(this, null)
    }

    fun updateSurface() {
        val surface = pres?.getSurfaceHolder()?.surface
        if (surface != null && surface.isValid) {
            onSurfaceUpdate(surface)
        } else {
            Log.warning("SecondaryDisplay Attempted to update null or invalid surface")
        }
    }

    fun destroySurface() {
        onSurfaceDestroy()
    }

    private fun getSecondaryDisplays(): List<Display> {
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val currentDisplayId = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            context.display.displayId
        } else {
            @Suppress("DEPRECATION")
            (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager)
                .defaultDisplay.displayId
        }
        val displays = dm.displays
        val presDisplays = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
        return displays.filter {
            val isPresentable = presDisplays.any { pd -> pd.displayId == it.displayId }
            val isNotDefaultOrPresentable =
                (it != null && it.displayId != Display.DEFAULT_DISPLAY) || isPresentable

            isNotDefaultOrPresentable &&
                it.displayId != currentDisplayId &&
                it.name != "HiddenDisplay" &&
                it.state != Display.STATE_OFF &&
                it.isValid
        }
    }

    fun updateDisplay() {
        // return early if the parent context is dead or dying
        if (context is android.app.Activity && (context.isFinishing || context.isDestroyed)) {
            return
        }

        val displayToUse = if (availableDisplays.isEmpty() ||
            // Theoretically, the NONE option is no longer selectable, but
            // I am leaving this in for backwards compatibility
            IntSetting.SECONDARY_DISPLAY_LAYOUT.int == SecondaryDisplayLayout.NONE.int ||
            !BooleanSetting.ENABLE_SECONDARY_DISPLAY.boolean
        ) {
            currentDisplayId = -1
            vd.display
        } else if (preferredDisplayId >= 0 &&
            availableDisplays.any { it.displayId == preferredDisplayId }
        ) {
            currentDisplayId = preferredDisplayId
            availableDisplays.first { it.displayId == preferredDisplayId }
        } else {
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val default = dm.displays.first { it.displayId == Display.DEFAULT_DISPLAY }
            // prioritize displays that have a different name from the default display, as
            // some devices such as the Odin 2 create a permanent virtual display with the same
            // name as the default display that should be skipped in most cases
            currentDisplayId = availableDisplays.firstOrNull {
                it.name != default.name && !it.name.contains("Built", true)
            }?.displayId
                ?: availableDisplays[0].displayId
            availableDisplays.first { it.displayId == currentDisplayId }
        }

        // if our presentation is already on the right display, ignore
        if (pres?.display == displayToUse) return

        // otherwise, make a new presentation
        releasePresentation()

        try {
            pres = SecondaryDisplayPresentation(context, displayToUse!!, this, onTouch, onTouchMoved)
            pres?.show()
        }
        // catch BadTokenException and InvalidDisplayException,
        // the display became invalid asynchronously, so we can assign to null
        // until onDisplayAdded/Removed/Changed is called and logic retriggered
        catch (_: WindowManager.BadTokenException) {
            pres = null
        } catch (_: WindowManager.InvalidDisplayException) {
            pres = null
        }
    }

    fun releasePresentation() {
        try {
            pres?.dismiss()
        } catch (_: Exception) { }
        pres = null
    }

    fun releaseVD() {
        displayManager.unregisterDisplayListener(this)
        vd.release()
    }

    override fun onDisplayAdded(displayId: Int) {
        updateDisplay()
    }

    override fun onDisplayRemoved(displayId: Int) {
        updateDisplay()
    }
    override fun onDisplayChanged(displayId: Int) {
        updateDisplay()
    }
}
class SecondaryDisplayPresentation(
    context: Context,
    display: Display,
    val parent: SecondaryDisplay,
    private val onTouch: (Float, Float, Boolean) -> Unit,
    private val onTouchMoved: (Float, Float) -> Unit
) : Presentation(context, display) {
    private lateinit var surfaceView: SurfaceView
    private var touchscreenPointerId = -1

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.setFlags(
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
        )

        // Initialize SurfaceView
        surfaceView = SurfaceView(context)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.debug("SecondaryDisplay Surface created")
            }

            override fun surfaceChanged(
                holder: SurfaceHolder,
                format: Int,
                width: Int,
                height: Int
            ) {
                Log.debug("SecondaryDisplay Surface changed: ${width}x$height")
                parent.updateSurface()
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.debug("SecondaryDisplay Surface destroyed")
                parent.destroySurface()
            }
        })

        this.surfaceView.setOnTouchListener { _, event ->

            val pointerIndex = event.actionIndex
            val pointerId = event.getPointerId(pointerIndex)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                    if (touchscreenPointerId == -1) {
                        touchscreenPointerId = pointerId
                        onTouch(event.getX(pointerIndex), event.getY(pointerIndex), true)
                    }
                }

                MotionEvent.ACTION_MOVE -> {
                    val index = event.findPointerIndex(touchscreenPointerId)
                    if (index != -1) {
                        onTouchMoved(event.getX(index), event.getY(index))
                    }
                }

                MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                    if (pointerId == touchscreenPointerId) {
                        onTouch(0f, 0f, false)
                        touchscreenPointerId = -1
                    }
                }
            }
            true
        }

        setContentView(surfaceView) // Set SurfaceView as content
    }

    // Publicly accessible method to get the SurfaceHolder
    fun getSurfaceHolder(): SurfaceHolder = surfaceView.holder
}
