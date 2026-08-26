// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.graphics.Bitmap
import java.io.File

data class NdsRom(val name: String, val absolutePath: String, val icon: Bitmap?) {
    companion object {
        /** Scans <profileDir>/sdmc/roms/{nds,dsi} for .nds/.dsi files, matching the desktop tool. */
        fun scan(profileDir: String): List<NdsRom> {
            val roms = mutableListOf<NdsRom>()
            for (subfolder in listOf("nds", "dsi")) {
                val dir = File(profileDir, "sdmc/roms/$subfolder")
                val files = dir.listFiles { f ->
                    f.isFile && (f.extension.equals("nds", true) || f.extension.equals("dsi", true))
                } ?: continue
                for (file in files.sortedBy { it.name.lowercase() }) {
                    roms.add(NdsRom(file.name, file.absolutePath, NdsIconDecoder.decode(file.absolutePath)))
                }
            }
            return roms
        }
    }
}
