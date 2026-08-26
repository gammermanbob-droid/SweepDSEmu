// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.content.Intent
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.widget.CheckBox
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import java.io.File

class MainActivity : AppCompatActivity() {
    private lateinit var romListView: RecyclerView
    private lateinit var forwarderListView: RecyclerView
    private lateinit var selectAllCheckbox: CheckBox
    private lateinit var installAfterBuildCheckbox: CheckBox
    private lateinit var buildButton: MaterialButton
    private var romAdapter: RomListAdapter? = null

    // Quiet, looping menu music for this screen (see README.md's Credits
    // section for attribution). Looked up by resource name at runtime
    // rather than a compile-time R.raw.menu_music reference so a
    // from-source build missing this one optional asset just silently
    // skips the music instead of failing to compile.
    private var menuMusicPlayer: MediaPlayer? = null

    companion object {
        private const val MENU_MUSIC_VOLUME = 0.25f
    }

    private val pickDirectory = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri == null) {
            if (PathUtil.getProfileDirectoryUri(this) == null) finish()
            return@registerForActivityResult
        }
        contentResolver.takePersistableUriPermission(
            uri,
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        )
        PathUtil.setProfileDirectoryUri(this, uri)
        if (PathUtil.getProfileDirectoryPath(this) == null) {
            val messageRes = if (PathUtil.isOnRemovableStorage(uri)) {
                R.string.ndsbrewer_directory_removable_storage
            } else {
                R.string.ndsbrewer_directory_invalid
            }
            Toast.makeText(this, messageRes, Toast.LENGTH_LONG).show()
            promptForDirectory()
        } else {
            refreshAll()
        }
    }

    private val manageStoragePermissionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            afterStoragePermissionCheck()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        romListView = findViewById(R.id.rom_list)
        romListView.layoutManager = LinearLayoutManager(this)
        forwarderListView = findViewById(R.id.forwarder_list)
        forwarderListView.layoutManager = LinearLayoutManager(this)
        selectAllCheckbox = findViewById(R.id.select_all)
        installAfterBuildCheckbox = findViewById(R.id.install_after_build)
        buildButton = findViewById(R.id.build_button)

        selectAllCheckbox.setOnCheckedChangeListener { _, checked ->
            romAdapter?.setAllChecked(checked)
        }
        buildButton.setOnClickListener { onBuildClicked() }

        ensureStoragePermission()
    }

    override fun onResume() {
        super.onResume()
        // Coming back from installing via the main app, or after a build:
        // keep the "manage" list honest.
        if (PathUtil.getProfileDirectoryPath(this) != null) {
            refreshForwarderList()
        }
        startMenuMusic()
    }

    override fun onPause() {
        // Pause rather than release/stop -- this Activity backgrounds
        // constantly for the directory picker, the storage-permission
        // Settings screen, and the install hand-off to the main app, all
        // of which return right back here rather than finishing it.
        menuMusicPlayer?.let { if (it.isPlaying) it.pause() }
        super.onPause()
    }

    override fun onDestroy() {
        menuMusicPlayer?.release()
        menuMusicPlayer = null
        super.onDestroy()
    }

    private fun startMenuMusic() {
        if (menuMusicPlayer != null) {
            menuMusicPlayer?.start()
            return
        }
        val resId = resources.getIdentifier("menu_music", "raw", packageName)
        if (resId == 0) {
            return
        }
        menuMusicPlayer = MediaPlayer.create(this, resId)?.apply {
            isLooping = true
            setVolume(MENU_MUSIC_VOLUME, MENU_MUSIC_VOLUME)
            setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            start()
        }
    }

    private fun hasManageExternalStorage(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    /**
     * The profile-directory SAF grant alone isn't enough:
     * ForwarderBuilder/ForwarderRegistry/NdsRom use plain java.io.File on
     * the real resolved path (see PathUtil), which needs
     * MANAGE_EXTERNAL_STORAGE the same way the main SweepDSEmu app's own
     * raw-filesystem-path features do. Ask for this first; the directory
     * picker only makes sense once it's granted.
     */
    private fun ensureStoragePermission() {
        if (hasManageExternalStorage()) {
            afterStoragePermissionCheck()
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.ndsbrewer_storage_permission_title)
            .setMessage(R.string.ndsbrewer_storage_permission_message)
            .setCancelable(false)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                manageStoragePermissionLauncher.launch(
                    Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.fromParts("package", packageName, null)
                    )
                )
            }
            .show()
    }

    private fun afterStoragePermissionCheck() {
        if (!hasManageExternalStorage()) {
            finish()
            return
        }
        val existing = PathUtil.getProfileDirectoryUri(this)
        if (existing == null || PathUtil.getProfileDirectoryPath(this) == null) {
            promptForDirectory()
        } else {
            refreshAll()
        }
    }

    private fun promptForDirectory() {
        AlertDialog.Builder(this)
            .setTitle(R.string.ndsbrewer_pick_directory_title)
            .setMessage(R.string.ndsbrewer_pick_directory_message)
            .setCancelable(false)
            .setPositiveButton(R.string.ndsbrewer_pick_directory_button) { _, _ ->
                pickDirectory.launch(null)
            }
            .show()
    }

    private fun refreshAll() {
        refreshRomList()
        refreshForwarderList()
    }

    private fun refreshRomList() {
        val profileDir = PathUtil.getProfileDirectoryPath(this) ?: return
        val roms = NdsRom.scan(profileDir)
        romAdapter = RomListAdapter(roms) { syncSelectAllCheckbox() }
        romListView.adapter = romAdapter
        selectAllCheckbox.isChecked = false
        if (roms.isEmpty()) {
            Toast.makeText(this, R.string.ndsbrewer_no_roms_found, Toast.LENGTH_SHORT).show()
        }
    }

    /**
     * Keeps the "Select All" checkbox reflecting reality after the user
     * checks/unchecks individual ROMs by hand -- e.g. ticking the last
     * remaining unchecked ROM should tick "Select All" too, and
     * unticking any one ROM should untick it. Toggling the listener off
     * for this one programmatic update avoids feeding back into
     * RomListAdapter.setAllChecked, which would just needlessly redo
     * work this same click already did.
     */
    private fun syncSelectAllCheckbox() {
        val adapter = romAdapter ?: return
        val allChecked = adapter.itemCount > 0 && adapter.checkedStates.all { it }
        selectAllCheckbox.setOnCheckedChangeListener(null)
        selectAllCheckbox.isChecked = allChecked
        selectAllCheckbox.setOnCheckedChangeListener { _, checked -> romAdapter?.setAllChecked(checked) }
    }

    private fun refreshForwarderList() {
        val profileDir = PathUtil.getProfileDirectoryPath(this) ?: return
        val forwarders = ForwarderRegistry.listForwarders(profileDir)
        forwarderListView.adapter = ForwarderListAdapter(forwarders) { forwarder ->
            confirmDelete(profileDir, forwarder)
        }
    }

    private fun confirmDelete(profileDir: String, forwarder: Forwarder) {
        val romName = File(forwarder.romPath).name
        AlertDialog.Builder(this)
            .setTitle(R.string.ndsbrewer_delete_confirm_title)
            .setMessage(getString(R.string.ndsbrewer_delete_confirm_message, romName))
            .setNegativeButton(R.string.ndsbrewer_cancel, null)
            .setPositiveButton(R.string.delete) { _, _ ->
                ForwarderRegistry.removeForwarder(profileDir, forwarder.programId)
                refreshForwarderList()
            }
            .show()
    }

    private fun onBuildClicked() {
        val profileDir = PathUtil.getProfileDirectoryPath(this) ?: return
        val selected = romAdapter?.selectedRoms().orEmpty()
        if (selected.isEmpty()) {
            Toast.makeText(this, R.string.ndsbrewer_no_roms_selected, Toast.LENGTH_SHORT).show()
            return
        }

        val installAfterBuild = installAfterBuildCheckbox.isChecked
        buildButton.isEnabled = false

        val progressView = layoutInflater.inflate(R.layout.dialog_build_progress, null)
        val progressLabel = progressView.findViewById<android.widget.TextView>(R.id.progress_label)
        val progressBar = progressView.findViewById<MartiniGlassProgressView>(R.id.progress_bar)
        val progressCount = progressView.findViewById<android.widget.TextView>(R.id.progress_count)
        progressBar.max = selected.size
        val progressDialog = AlertDialog.Builder(this)
            .setTitle(R.string.ndsbrewer_build_button)
            .setView(progressView)
            .setCancelable(false)
            .create()
        progressDialog.show()

        Thread {
            var successCount = 0
            val failures = mutableListOf<String>()
            val builtCias = mutableListOf<String>()

            selected.forEachIndexed { index, rom ->
                runOnUiThread {
                    progressLabel.text = getString(R.string.ndsbrewer_building, rom.name)
                    progressCount.text = getString(
                        R.string.ndsbrewer_build_progress_count,
                        index + 1,
                        selected.size
                    )
                    progressBar.progress = index
                }
                var error: String? = null
                val ciaPath = ForwarderBuilder.build(this, profileDir, rom.absolutePath) { error = it }
                if (ciaPath != null) {
                    successCount++
                    builtCias.add(ciaPath)
                } else {
                    failures.add(getString(R.string.ndsbrewer_build_failed, rom.name, error ?: "unknown error"))
                }
                runOnUiThread { progressBar.progress = index + 1 }
            }

            var notInstalledCount = 0
            var permissionDeniedCount = 0
            if (installAfterBuild && builtCias.isNotEmpty()) {
                runOnUiThread {
                    progressLabel.text = getString(R.string.ndsbrewer_installing)
                }
                for (ciaPath in builtCias) {
                    when (InstallBridge.installCia(this, ciaPath)) {
                        InstallBridge.Result.LAUNCHED -> {
                            // Deliberately not deleting ciaPath here: the
                            // actual install runs asynchronously in the
                            // main app's own process (InstallCiaBridge
                            // Activity -> CiaInstallWorker via WorkManager),
                            // so this process has no way to know when the
                            // file has actually been read. That app deletes
                            // it once WorkManager reports the install
                            // finished -- deleting it here immediately
                            // raced that job and deleted the file before it
                            // was ever opened.
                        }
                        InstallBridge.Result.NOT_INSTALLED -> notInstalledCount++
                        InstallBridge.Result.PERMISSION_DENIED -> permissionDeniedCount++
                    }
                }
            }

            runOnUiThread {
                progressDialog.dismiss()
                buildButton.isEnabled = true
                if (notInstalledCount > 0) {
                    Toast.makeText(this, R.string.ndsbrewer_install_not_installed, Toast.LENGTH_LONG).show()
                }
                if (permissionDeniedCount > 0) {
                    Toast.makeText(this, R.string.ndsbrewer_install_permission_denied, Toast.LENGTH_LONG).show()
                }
                if (failures.isEmpty()) {
                    Toast.makeText(
                        this,
                        getString(R.string.ndsbrewer_build_done, successCount),
                        Toast.LENGTH_LONG
                    ).show()
                } else {
                    AlertDialog.Builder(this)
                        .setMessage(
                            getString(
                                R.string.ndsbrewer_build_done_with_failures,
                                successCount,
                                failures.size,
                                failures.joinToString("\n")
                            )
                        )
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
                refreshForwarderList()
            }
        }.start()
    }
}
