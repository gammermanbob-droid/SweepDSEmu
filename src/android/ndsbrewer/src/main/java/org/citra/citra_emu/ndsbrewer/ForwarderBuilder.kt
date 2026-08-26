// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest

data class ForwarderIdentity(val uniqueId: Int, val productCode: String, val programId: Long)

/**
 * Kotlin port of src/ndsbrewer_meta/ndsbrewer_window.cpp's
 * ComputeIdentity/BuildForwarderCia -- same SHA-256-of-the-absolute-ROM-
 * path derivation and the same bannertool/makerom command lines, so a
 * forwarder built here gets the exact same title ID a desktop-built one
 * for the same ROM path would.
 */
object ForwarderBuilder {
    private const val ICON_SIZE = 48 // SMDH icon size bannertool expects.

    fun computeIdentity(absoluteRomPath: String): ForwarderIdentity {
        val digest = MessageDigest.getInstance("SHA-256").digest(absoluteRomPath.toByteArray(Charsets.UTF_8))
        var uniqueId = ((digest[0].toUByte().toInt() shl 8) or digest[1].toUByte().toInt())
        if (uniqueId == 0) uniqueId = 1
        val productCode = "DSF%02X".format(digest[0].toUByte().toInt())
        // makerom builds the real title ID's low 32 bits as (UniqueId << 8) |
        // variation (variation 0x00 for a plain application) -- the registry
        // has to record that same combined value, since that's what
        // ReadProgramId() actually reports back for the installed CIA.
        val programId = 0x0004000000000000L or (uniqueId.toLong() shl 8)
        return ForwarderIdentity(uniqueId, productCode, programId)
    }

    private fun safeFileName(title: String): String =
        title.map { c -> if (c.isLetterOrDigit() || c == ' ' || c == '-' || c == '_') c else '_' }
            .joinToString("").trim()

    // A zero-length WAV data chunk is technically valid, but bannertool's
    // banner audio is meant to loop for as long as an icon stays
    // highlighted on the HOME Menu -- a real (if short) run of true-zero
    // 16-bit PCM samples is unambiguous silence either way a decoder
    // handles an empty one. Mirrors ndsbrewer_window.cpp's BuildSilentWav
    // (itself mirroring tools/make_ds_forwarder.py's _build_silent_wav()).
    private fun buildSilentWav(): ByteArray {
        val sampleRate = 32728
        val numSamples = sampleRate // 1.0 second
        val pcm = ByteArray(numSamples * 2)

        fun u16(v: Int) = byteArrayOf((v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte())
        fun u32(v: Int) = byteArrayOf(
            (v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(), ((v shr 24) and 0xFF).toByte()
        )

        val fmtChunk = u16(1) + u16(1) + u32(sampleRate) + u32(sampleRate * 2) + u16(2) + u16(16)
        val riffSize = 4 + (8 + fmtChunk.size) + (8 + pcm.size)

        return "RIFF".toByteArray() + u32(riffSize) + "WAVE".toByteArray() +
            "fmt ".toByteArray() + u32(fmtChunk.size) + fmtChunk +
            "data".toByteArray() + u32(pcm.size) + pcm
    }

    private fun runTool(program: String, args: List<String>, workDir: File): String? {
        val process = try {
            val builder = ProcessBuilder(listOf(program) + args)
                .directory(workDir)
                .redirectErrorStream(true)
            // The dynamic linker doesn't search an executable's own
            // directory by default (only RPATH/RUNPATH baked into the
            // binary, LD_LIBRARY_PATH, or the standard system paths) --
            // bannertool/makerom need libc++_shared.so, which is packaged
            // right alongside them in this same nativeLibraryDir, so it
            // has to be pointed at explicitly.
            builder.environment()["LD_LIBRARY_PATH"] = File(program).parent
            builder.start()
        } catch (e: Exception) {
            return "${File(program).name} failed to start: ${e.message}"
        }
        val output = process.inputStream.bufferedReader().readText()
        val finished = process.waitFor(30, java.util.concurrent.TimeUnit.SECONDS)
        if (!finished) {
            process.destroyForcibly()
            return "${File(program).name} did not finish in time"
        }
        if (process.exitValue() != 0) {
            return "${File(program).name} failed (exit ${process.exitValue()}):\n$output"
        }
        return null
    }

    /** Returns the built .cia's path, or null (with [onError] called) on failure. */
    fun build(
        context: Context,
        profileDir: String,
        absoluteRomPath: String,
        onError: (String) -> Unit
    ): String? {
        val icon = NdsIconDecoder.decode(absoluteRomPath)
        if (icon == null) {
            onError("This ROM has no icon/banner block -- can't build a forwarder for it.")
            return null
        }

        val workDir = File(context.cacheDir, "ndsbrewer_work_${System.currentTimeMillis()}")
        workDir.mkdirs()
        try {
            // Nearest-neighbor upscale from the native 32x32 keeps the
            // pixelated look instead of blurring it, matching
            // make_ds_forwarder.py.
            val iconScaled = Bitmap.createScaledBitmap(icon, ICON_SIZE, ICON_SIZE, false)
            val iconPng = File(workDir, "icon.png")
            iconPng.outputStream().use { iconScaled.compress(Bitmap.CompressFormat.PNG, 100, it) }

            val stubElf = File(workDir, "forwarder.elf")
            context.resources.openRawResource(R.raw.ds_forwarder_stub).use { input ->
                stubElf.outputStream().use { input.copyTo(it) }
            }

            val title = File(absoluteRomPath).nameWithoutExtension
            val nativeLibDir = context.applicationInfo.nativeLibraryDir
            val bannertool = File(nativeLibDir, "libndsbrewer_bannertool.so")
            val makerom = File(nativeLibDir, "libndsbrewer_makerom.so")
            val rsfTemplate = File(workDir, "template.rsf")
            context.assets.open("template.rsf").use { input ->
                rsfTemplate.outputStream().use { input.copyTo(it) }
            }
            if (!bannertool.isFile || !makerom.isFile) {
                onError("bannertool/makerom weren't found -- reinstall SweepDSEmuNDSBrewer.")
                return null
            }
            bannertool.setExecutable(true)
            makerom.setExecutable(true)

            val smdhPath = File(workDir, "forwarder.smdh")
            runTool(
                bannertool.path,
                listOf(
                    "makesmdh", "-s", title, "-l", title, "-p", "Azahar",
                    "-i", iconPng.path, "-o", smdhPath.path, "-f", "visible,extendedbanner"
                ),
                workDir
            )?.let { onError(it); return null }

            // 256x128 fixed canvas: no cover art input in this first
            // version, so just center the icon on a plain background
            // (matches make_ds_forwarder.py's own no-cover-art fallback).
            val bannerImage = Bitmap.createBitmap(256, 128, Bitmap.Config.ARGB_8888)
            Canvas(bannerImage).apply {
                drawColor(Color.rgb(32, 32, 48))
                val bannerIcon = Bitmap.createScaledBitmap(icon, 96, 96, false)
                drawBitmap(
                    bannerIcon,
                    (256 - bannerIcon.width) / 2f,
                    (128 - bannerIcon.height) / 2f,
                    Paint()
                )
            }
            val bannerPng = File(workDir, "banner.png")
            bannerPng.outputStream().use { bannerImage.compress(Bitmap.CompressFormat.PNG, 100, it) }

            val silenceWav = File(workDir, "silence.wav")
            silenceWav.writeBytes(buildSilentWav())

            val bannerBnr = File(workDir, "forwarder.bnr")
            runTool(
                bannertool.path,
                listOf("makebanner", "-i", bannerPng.path, "-a", silenceWav.path, "-o", bannerBnr.path),
                workDir
            )?.let { onError(it); return null }

            val identity = computeIdentity(absoluteRomPath)
            val emptyRomfs = File(workDir, "empty_romfs")
            emptyRomfs.mkdirs()

            val outputCiaDir = File(profileDir, "ndsbrewer_output")
            outputCiaDir.mkdirs()
            val outputCia = File(outputCiaDir, "${safeFileName(title)}.cia")

            runTool(
                makerom.path,
                listOf(
                    "-f", "cia", "-o", outputCia.path,
                    "-rsf", rsfTemplate.path, "-elf", stubElf.path,
                    "-icon", smdhPath.path, "-banner", bannerBnr.path,
                    "-target", "t", "-exefslogo",
                    "-DAPP_TITLE=$title",
                    "-DAPP_PRODUCT_CODE=${identity.productCode}",
                    "-DAPP_ROMFS=${emptyRomfs.path}",
                    "-DAPP_CATEGORY=Application",
                    "-DAPP_UNIQUE_ID=${identity.uniqueId}",
                    "-DAPP_USE_ON_SD=true", "-DAPP_ENCRYPTED=false",
                    "-DAPP_MEMORY_TYPE=Application",
                    "-DAPP_SYSTEM_MODE=64MB",
                    "-DAPP_SYSTEM_MODE_EXT=Legacy",
                    "-DAPP_CPU_SPEED=268MHz",
                    "-DAPP_ENABLE_L2_CACHE=false",
                    "-DAPP_VERSION_MAJOR=1"
                ),
                workDir
            )?.let { onError(it); return null }

            ForwarderRegistry.registerForwarder(profileDir, identity.programId, absoluteRomPath)
            return outputCia.path
        } finally {
            workDir.deleteRecursively()
        }
    }
}
