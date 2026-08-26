// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import java.io.File

data class Forwarder(val programId: Long, val romPath: String)

/**
 * Kotlin port of src/citra_qt/ds_forwarder_registry.cpp / native.cpp's
 * ResolveAndroidDSForwarder -- same flat
 * "<16 lowercase hex program_id>=<DS ROM path>" ds_forwarders.txt format,
 * with the same UserDir-relative-when-possible path convention (see
 * ToStoredPath/FromStoredPath there) so a forwarder built here resolves
 * identically whether the profile directory is later used from this app,
 * the main SweepDSEmu app, or a desktop profile copied onto the device.
 */
object ForwarderRegistry {
    private fun registryFile(profileDir: String) = File(profileDir, "ds_forwarders.txt")

    private fun toStoredPath(profileDir: String, absoluteRomPath: String): String {
        val dir = File(profileDir).canonicalPath
        val rom = File(absoluteRomPath).canonicalPath
        return if (rom.startsWith("$dir/")) rom.substring(dir.length + 1) else rom
    }

    private fun fromStoredPath(profileDir: String, stored: String): String =
        if (stored.startsWith("/")) stored else "$profileDir/$stored"

    fun listForwarders(profileDir: String): List<Forwarder> {
        val file = registryFile(profileDir)
        if (!file.isFile) return emptyList()

        val forwarders = mutableListOf<Forwarder>()
        file.forEachLine { rawLine ->
            val line = rawLine.trim()
            val equals = line.indexOf('=')
            if (equals <= 0) return@forEachLine
            val programId = line.substring(0, equals).toLongOrNull(16) ?: return@forEachLine
            forwarders.add(Forwarder(programId, fromStoredPath(profileDir, line.substring(equals + 1))))
        }
        return forwarders
    }

    fun registerForwarder(profileDir: String, programId: Long, absoluteRomPath: String) {
        val needle = "%016x=".format(programId)
        val file = registryFile(profileDir)
        val remaining = if (file.isFile) {
            file.readLines().filterNot { it.startsWith(needle, ignoreCase = true) }
        } else {
            emptyList()
        }
        file.writeText((remaining + (needle + toStoredPath(profileDir, absoluteRomPath)))
            .joinToString("\n", postfix = "\n"))
    }

    /**
     * Un-registers a forwarder and undoes everything CIA installation did
     * for it, mirroring DSForwarderRegistry::RemoveForwarder exactly: the
     * SD title's own content directory and its ticket file under
     * nand/dbs/ticket.db/ (matched by the program_id prefix, glob-matched
     * in both cases since installed ticket filenames are uppercase hex
     * while registered program IDs are written lowercase). Returns false
     * if program_id wasn't registered; best-effort cleanup still runs
     * either way.
     */
    fun removeForwarder(profileDir: String, programId: Long): Boolean {
        val needle = "%016x=".format(programId)
        val file = registryFile(profileDir)
        var found = false
        val remaining = if (file.isFile) {
            file.readLines().filterNot {
                val matches = it.startsWith(needle, ignoreCase = true)
                if (matches) found = true
                matches
            }
        } else {
            emptyList()
        }

        // Low 32 bits of program_id as 8 hex digits, matching
        // Service::AM::GetTitlePath's own title-id-low formatting.
        val titleIdLow = "%08x".format(programId and 0xFFFFFFFFL)
        val contentDir = File(
            profileDir,
            "sdmc/Nintendo 3DS/00000000000000000000000000000000/" +
                "00000000000000000000000000000000/title/00040000/$titleIdLow"
        )
        contentDir.deleteRecursively()

        val ticketDir = File(profileDir, "nand/dbs/ticket.db")
        val programIdHex = "%016x".format(programId)
        ticketDir.listFiles { f ->
            f.name.startsWith(programIdHex, ignoreCase = true) && f.name.endsWith(".tik")
        }?.forEach { it.delete() }

        if (found) {
            file.writeText(remaining.joinToString("\n", postfix = if (remaining.isEmpty()) "" else "\n"))
        }
        return found
    }
}
