// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.utils

import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.work.Data
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequest
import androidx.work.OutOfQuotaPolicy
import androidx.work.WorkInfo
import androidx.work.WorkManager
import org.citra.citra_emu.R
import java.io.File

/**
 * No-UI entry point that lets SweepDSEmuNDSBrewer -- a separate APK, so it
 * can't call CiaInstallWorker or any other in-process API directly --
 * install a forwarder .cia it just built into this app's own profile.
 * Guarded by the signature-level org.citra.citra_emu.permission.
 * INSTALL_CIA_BRIDGE permission (see AndroidManifest.xml), so only an app
 * signed with the same key can reach this.
 *
 * [EXTRA_CIA_PATH] is a real, already-resolved absolute filesystem path
 * (NDSBrewer writes its output directly under the shared profile
 * directory, the same one this app's own MANAGE_EXTERNAL_STORAGE access
 * already reads/writes freely). CiaInstallWorker.doWork() expects each
 * "CIA_FILES" entry to be a parseable URI it resolves itself (applying
 * its own "!" + getNativePath(...) prefixing internally) -- so this is
 * handed over as a plain file:// URI, which NativeLibrary.getNativePath's
 * uri.scheme == "file" branch turns back into this same real path.
 *
 * Unlike the in-app "Install CIA" picker (which never deletes the user's
 * own file), this deletes [EXTRA_CIA_PATH] once WorkManager reports the
 * install finished -- it's NDSBrewer's own disposable, freshly-generated
 * copy, matching desktop NDSBrewer's "install then delete .cia" behavior.
 * That deletion has to happen here, from this process, only after the
 * work actually reaches a terminal state: NDSBrewer's own process can't
 * see when CiaInstallWorker (running over here, asynchronously) has
 * actually finished reading the file, so it can't safely delete its copy
 * itself -- deleting right after startActivity() returns raced the
 * WorkManager job and deleted the file out from under it before it was
 * ever read.
 */
class InstallCiaBridgeActivity : AppCompatActivity() {
    companion object {
        const val EXTRA_CIA_PATH = "CIA_PATH"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (BuildUtil.isGooglePlayBuild) {
            // The raw-filesystem-path install path this bridges into
            // doesn't apply on the Play flavor (see CiaInstallWorker.
            // doWork()'s own isGooglePlayBuild branch, which expects a
            // real content:// URI it already has read access to instead)
            // -- NDSBrewer's whole install hand-off is an F-Droid/sideload
            // feature, matching every other CanUseRawFS()-gated piece of
            // this codebase.
            Toast.makeText(this, R.string.ndsbrewer_install_unsupported_google_play, Toast.LENGTH_LONG)
                .show()
            finish()
            return
        }

        val ciaPath = intent.getStringExtra(EXTRA_CIA_PATH)
        if (ciaPath.isNullOrEmpty() || !File(ciaPath).isFile) {
            Toast.makeText(this, R.string.cia_file_not_found, Toast.LENGTH_LONG).show()
            finish()
            return
        }

        val fileUri = android.net.Uri.fromFile(File(ciaPath)).toString()
        val request = OneTimeWorkRequest.Builder(CiaInstallWorker::class.java)
            .setInputData(
                Data.Builder().putStringArray("CIA_FILES", arrayOf(fileUri))
                    .build()
            )
            .setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST)
            .build()

        val workManager = WorkManager.getInstance(applicationContext)
        workManager.enqueueUniqueWork("installCiaWork", ExistingWorkPolicy.APPEND_OR_REPLACE, request)
        workManager.getWorkInfoByIdLiveData(request.id).observe(this) { info ->
            if (info != null && info.state.isFinished) {
                if (info.state == WorkInfo.State.SUCCEEDED) {
                    File(ciaPath).delete()
                }
                finish()
            }
        }
    }
}
