// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Hands a freshly built .cia off to the main SweepDSEmu app for actual
 * installation, since that's a separate APK/process -- see
 * InstallCiaBridgeActivity in that app's own module for the receiving
 * end. Tries each known applicationId variant in turn (vanilla, its debug
 * suffix, the AYN Thor build) since NDSBrewer has no way to know which
 * one, if any, the user has installed.
 */
object InstallBridge {
    private const val BRIDGE_ACTIVITY = "org.citra.citra_emu.utils.InstallCiaBridgeActivity"
    private const val EXTRA_CIA_PATH = "CIA_PATH"

    private val candidatePackages = listOf(
        "io.github.lime3ds.android",
        "io.github.lime3ds.android.debug",
        "io.github.lime3ds.android.thor"
    )

    enum class Result { LAUNCHED, NOT_INSTALLED, PERMISSION_DENIED }

    fun installCia(context: Context, ciaPath: String): Result {
        var sawPermissionDenied = false
        for (packageName in candidatePackages) {
            val intent = Intent().apply {
                setClassName(packageName, BRIDGE_ACTIVITY)
                putExtra(EXTRA_CIA_PATH, ciaPath)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            try {
                context.startActivity(intent)
                return Result.LAUNCHED
            } catch (e: ActivityNotFoundException) {
                // Try the next candidate package.
            } catch (e: SecurityException) {
                // This package is installed but isn't signed with the
                // same key as this build of NDSBrewer -- startActivity()
                // throws synchronously for a missing signature-level
                // permission, so this has to be caught here rather than
                // left to crash the caller. Keep trying other candidates
                // (e.g. a signed-matching build under a different variant
                // suffix) before giving up.
                Log.w("InstallBridge", "Signature mismatch launching bridge in $packageName", e)
                sawPermissionDenied = true
            }
        }
        return if (sawPermissionDenied) Result.PERMISSION_DENIED else Result.NOT_INSTALLED
    }
}
