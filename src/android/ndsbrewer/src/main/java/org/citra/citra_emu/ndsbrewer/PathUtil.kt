// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.content.Context
import android.net.Uri
import android.os.Environment
import androidx.documentfile.provider.DocumentFile

/**
 * NDSBrewer is a separate app/process from the main SweepDSEmu app, so a
 * SAF tree permission grant on one side never carries over to the other --
 * the user has to pick their profile directory again here (see
 * MainActivity's first-run flow), even if it's the exact same real folder
 * they already picked in the main app.
 *
 * Once granted here, this mirrors NativeLibrary.getNativePath()'s own
 * "primary:" trick (src/android/app/src/main/java/org/citra/citra_emu/
 * NativeLibrary.kt) for turning that tree URI into a real filesystem path
 * bannertool/makerom (plain native processes, no SAF awareness at all) can
 * open directly -- rather than porting the full DocumentsTree/content-URI
 * layer just for this. Only primary external storage is handled; a
 * profile directory on a removable SD card isn't resolvable to a raw path
 * this way and isn't supported in this first version.
 */
object PathUtil {
    private const val PREFS_NAME = "ndsbrewer_prefs"
    private const val KEY_PROFILE_DIRECTORY = "profile_directory"

    fun getProfileDirectoryUri(context: Context): Uri? {
        val stored = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_PROFILE_DIRECTORY, null) ?: return null
        return Uri.parse(stored)
    }

    fun setProfileDirectoryUri(context: Context, uri: Uri) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString(KEY_PROFILE_DIRECTORY, uri.toString())
            .apply()
    }

    /** Null if the picked directory isn't on primary storage (unsupported) or isn't set yet. */
    fun getProfileDirectoryPath(context: Context): String? {
        val uri = getProfileDirectoryUri(context) ?: return null
        val docId = try {
            android.provider.DocumentsContract.getTreeDocumentId(uri)
        } catch (e: Exception) {
            return null
        }
        if (!docId.startsWith("primary:")) {
            return null
        }
        val relative = docId.substringAfter(":")
        val primaryStoragePath = Environment.getExternalStorageDirectory().absolutePath
        return if (relative.isEmpty()) primaryStoragePath else "$primaryStoragePath/$relative"
    }

    fun directoryLooksValid(context: Context, uri: Uri): Boolean {
        val root = DocumentFile.fromTreeUri(context, uri) ?: return false
        return root.exists() && root.isDirectory
    }
}
