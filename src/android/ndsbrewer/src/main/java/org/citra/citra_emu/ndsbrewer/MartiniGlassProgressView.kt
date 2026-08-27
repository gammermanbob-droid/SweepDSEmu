// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.content.Context
import android.graphics.Color
import android.util.AttributeSet
import android.view.ViewGroup
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout

/**
 * NDSBrewer's build-progress indicator: a martini glass that fills up as
 * ROMs are processed, in place of a plain ProgressBar/LinearProgressIndicator.
 * The glass itself is assets/martini_progress.html (gradients, sparkle/bubble
 * animation, and a fruit garnish are far easier to keep faithful to the
 * original design as SVG+CSS than to re-derive in Canvas), driven here via
 * its window.setLoadingProgress(0-100) JS API.
 *
 * Wraps the WebView in a plain FrameLayout, rather than subclassing WebView
 * directly, purely so this class's own "progress"/"max" properties (mirroring
 * ProgressBar's API, for a drop-in replacement in MainActivity's existing
 * progress-dialog code) don't collide with WebView's own unrelated
 * getProgress() (page-load percentage).
 */
class MartiniGlassProgressView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : FrameLayout(context, attrs) {

    private val webView: WebView
    private var pageReady = false
    private var pendingPercent: Int? = null

    var max: Int = 100
        set(value) {
            field = value.coerceAtLeast(1)
            pushProgress()
        }

    var progress: Int = 0
        set(value) {
            field = value.coerceIn(0, max)
            pushProgress()
        }

    init {
        webView = WebView(context).apply {
            layoutParams = LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            setBackgroundColor(Color.TRANSPARENT)
            isVerticalScrollBarEnabled = false
            isHorizontalScrollBarEnabled = false
            overScrollMode = OVER_SCROLL_NEVER
            settings.javaScriptEnabled = true
            settings.setSupportZoom(false)
            settings.builtInZoomControls = false
            webViewClient = object : WebViewClient() {
                override fun onPageFinished(view: WebView?, url: String?) {
                    pageReady = true
                    pendingPercent?.let { runSetProgress(it) }
                    pendingPercent = null
                }
            }
            loadUrl("file:///android_asset/martini_progress.html")
        }
        addView(webView)
    }

    private fun pushProgress() {
        val percent = (100 * progress / max).coerceIn(0, 100)
        if (pageReady) {
            runSetProgress(percent)
        } else {
            pendingPercent = percent
        }
    }

    private fun runSetProgress(percent: Int) {
        webView.evaluateJavascript("window.setLoadingProgress($percent)", null)
    }
}
