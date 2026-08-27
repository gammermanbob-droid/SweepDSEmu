// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.model

import android.content.Intent
import android.net.Uri
import android.os.Parcelable
import androidx.core.net.toUri
import java.io.File
import java.io.IOException
import java.util.HashSet
import kotlinx.parcelize.Parcelize
import kotlinx.serialization.Serializable
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.activities.EmulationActivity
import org.citra.citra_emu.utils.BuildUtil

@Parcelize
@Serializable
class Game(
    val valid: Boolean = false,
    val title: String = "",
    val description: String = "",
    val path: String = "",
    val titleId: Long = 0L,
    val mediaType: MediaType = MediaType.GAME_CARD,
    val company: String = "",
    val regions: String = "",
    val isInstalled: Boolean = false,
    val isSystemTitle: Boolean = false,
    val isVisibleSystemTitle: Boolean = false,
    val isInsertable: Boolean = false,
    val icon: IntArray? = null,
    val fileType: String = "",
    val isCompressed: Boolean = false,
    val filename: String
) : Parcelable {
    val keyAddedToLibraryTime get() = "${filename}_AddedToLibraryTime"
    val keyLastPlayedTime get() = "${filename}_LastPlayed"

    val launchIntent: Intent
        get() {
            var appUri: Uri
            if (isInstalled) {
                if (BuildUtil.isGooglePlayBuild) {
                    appUri = CitraApplication.documentsTree.getUri(path)
                } else {
                    val nativePath = NativeLibrary.getUserDirectory() + "/" + path
                    val nativeFile = File(nativePath)
                    if (!nativeFile.exists()) {
                        throw IOException(
                            "Attempting to create shortcut for an executable that doesn't exist: $nativePath"
                        )
                    }
                    appUri = Uri.fromFile(nativeFile)
                }
            } else {
                appUri = path.toUri()
            }
            return Intent(CitraApplication.appContext, EmulationActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                data = appUri
            }
        }

    override fun equals(other: Any?): Boolean {
        if (other !is Game) {
            return false
        }

        return hashCode() == other.hashCode()
    }

    override fun hashCode(): Int {
        var result = title.hashCode()
        result = 31 * result + description.hashCode()
        result = 31 * result + regions.hashCode()
        result = 31 * result + path.hashCode()
        result = 31 * result + titleId.hashCode()
        result = 31 * result + mediaType.hashCode()
        result = 31 * result + company.hashCode()
        return result
    }

    enum class MediaType(val value: Int) {
        NAND(0),
        SDMC(1),
        GAME_CARD(2);

        companion object {
            fun fromInt(value: Int): MediaType? = MediaType.entries.find { it.value == value }
        }
    }

    companion object {
        val allExtensions: Set<String> get() = extensions + badExtensions

        // DS/DSi ROM extensions -- their own set (rather than just being
        // inline in `extensions` below) so GameHelper can filter them out
        // by extension alone when Settings.KEY_DS_HIDE_FROM_GAME_LIST is on.
        val dsExtensions: Set<String> = HashSet(listOf("nds", "dsi"))

        val extensions: Set<String> = HashSet(
            listOf(
                "3dsx", "app", "axf", "cci", "cxi", "elf", "z3dsx", "zcci", "zcxi", "3ds"
            ) + dsExtensions
            // GameInfo's native NCCH/SMDH loader doesn't understand the DS/DSi
            // extensions above (isValid() comes back false, same as any other
            // unrecognized file), but that only affects the long-press "about
            // game" dialog; the list still shows them under their filename and
            // EmulationActivity already redirects a direct .nds/.dsi tap to
            // DsEmulationActivity.
        )

        val badExtensions: Set<String> = HashSet(
            listOf("rar", "7z", "torrent", "tar", "gz")
        )
    }
}
