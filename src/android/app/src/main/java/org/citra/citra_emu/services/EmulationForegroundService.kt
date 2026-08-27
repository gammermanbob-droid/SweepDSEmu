// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.services

import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import org.citra.citra_emu.R

/**
 * Keeps an emulation session (3DS or DS) alive while its Activity is
 * merely backgrounded (e.g. briefly switching to another app or going to
 * the home screen) by running as a foreground service with a persistent
 * low-priority notification. Without this, Android's background process
 * killer can and does reclaim the whole process under memory pressure --
 * especially on OEM skins with aggressive background-app policies -- which
 * silently discards the entire running session, making the game appear to
 * "reset" back to power-on the next time its Activity is reopened. Cart/
 * game save data alone already survives that independently (autosave on
 * both cores); this is about keeping the actual in-progress session alive
 * too, not just the save file. Started by EmulationFragment/
 * DsEmulationActivity when emulation truly begins (not on every resume)
 * and stopped when it actually ends (user exits, ROM finishes, crash) --
 * never merely on backgrounding.
 *
 * Known limitation: this only protects against the OS treating the app as
 * an idle background process -- it can't stop Android's low-memory killer
 * from reclaiming the process under genuine memory pressure (confirmed via
 * `adb shell dumpsys deviceidle whitelist` / `am get-standby-bucket`
 * already showing this app correctly exempted from Doze/App-Standby
 * restrictions in that scenario, and device logs showing `lmkd` actively
 * reclaiming other apps' memory at the same time). 3DS emulation's memory
 * footprint (texture/shader caches, JIT code, audio/video buffers) makes it
 * a natural target once RAM is genuinely tight from heavy multitasking.
 * There is no further app-side fix for this -- it's a hard Android
 * constraint, not a bug in this service.
 */
class EmulationForegroundService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val title = intent?.getStringExtra(EXTRA_TITLE) ?: getString(R.string.app_name)
        val reopenIntent = intent?.getParcelableExtra(EXTRA_REOPEN_INTENT, Intent::class.java)
        val builder = NotificationCompat.Builder(
            this,
            getString(R.string.app_notification_channel_id)
        )
            .setContentTitle(title)
            .setContentText(getString(R.string.emulation_running_notification_text))
            .setSmallIcon(R.drawable.ic_stat_notification_logo)
            .setOngoing(true)
        if (reopenIntent != null) {
            builder.setContentIntent(
                PendingIntent.getActivity(
                    this,
                    0,
                    reopenIntent,
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                )
            )
        }
        val notification = builder.build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        return START_NOT_STICKY
    }

    companion object {
        private const val NOTIFICATION_ID = 0x44530001
        const val EXTRA_TITLE = "Title"
        const val EXTRA_REOPEN_INTENT = "ReopenIntent"
    }
}
