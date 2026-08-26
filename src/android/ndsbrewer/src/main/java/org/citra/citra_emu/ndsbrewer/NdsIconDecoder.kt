// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.ndsbrewer

import android.graphics.Bitmap
import android.graphics.Color
import java.io.RandomAccessFile

/**
 * Kotlin port of src/citra_qt/util/nds_icon.cpp's Decode() -- same banner
 * offset/bitmap/palette layout, just read with RandomAccessFile instead of
 * std::ifstream since this module has no Qt (or C++) to share it with
 * directly.
 */
object NdsIconDecoder {
    private const val BANNER_OFFSET_FIELD_OFFSET = 0x68 // u32 LE, per NDS_Header.h's BannerOffset
    private const val BITMAP_OFFSET = 0x20 // relative to the banner itself
    private const val PALETTE_OFFSET = 0x220
    private const val BITMAP_SIZE = 512 // 32x32 px, 4bpp
    private const val PALETTE_SIZE = 32 // 16 entries, u16 each
    private const val ICON_DIM = 32
    private const val TILE_DIM = 8
    private const val TILES_PER_ROW = ICON_DIM / TILE_DIM // 4

    // DS colors are 5-bit-per-channel RGB555 (bit15 unused); scale each
    // channel to 8 bits by replicating the top bits into the low ones
    // rather than a plain *255/31, which visibly darkens saturated colors.
    private fun scale5To8(v5: Int): Int = (v5 shl 3) or (v5 shr 2)

    /** Null if this ROM has no icon/banner block. */
    fun decode(romPath: String): Bitmap? {
        RandomAccessFile(romPath, "r").use { file ->
            file.seek(BANNER_OFFSET_FIELD_OFFSET.toLong())
            val offsetBytes = ByteArray(4)
            if (file.read(offsetBytes) != 4) return null
            val bannerOffset = (offsetBytes[0].toUByte().toLong()) or
                (offsetBytes[1].toUByte().toLong() shl 8) or
                (offsetBytes[2].toUByte().toLong() shl 16) or
                (offsetBytes[3].toUByte().toLong() shl 24)
            if (bannerOffset == 0L) return null // No banner in this ROM.

            val bitmap = ByteArray(BITMAP_SIZE)
            file.seek(bannerOffset + BITMAP_OFFSET)
            if (file.read(bitmap) != BITMAP_SIZE) return null

            val paletteBytes = ByteArray(PALETTE_SIZE)
            file.seek(bannerOffset + PALETTE_OFFSET)
            if (file.read(paletteBytes) != PALETTE_SIZE) return null

            // 16 palette entries, RGB555 LE. Index 0 is always
            // transparent, regardless of whatever color value happens to
            // be stored there.
            val palette = IntArray(16)
            for (i in 0 until 16) {
                val raw = (paletteBytes[i * 2].toUByte().toInt()) or
                    (paletteBytes[i * 2 + 1].toUByte().toInt() shl 8)
                val r = scale5To8(raw and 0x1F)
                val g = scale5To8((raw shr 5) and 0x1F)
                val b = scale5To8((raw shr 10) and 0x1F)
                palette[i] = if (i == 0) Color.TRANSPARENT else Color.argb(255, r, g, b)
            }

            val result = Bitmap.createBitmap(ICON_DIM, ICON_DIM, Bitmap.Config.ARGB_8888)

            // The bitmap is a 4x4 grid of 8x8 tiles; within a tile, rows
            // run top to bottom and each row's 8 pixels are packed
            // 2-per-byte (low nibble = left pixel, high nibble = right
            // pixel).
            for (tile in 0 until TILES_PER_ROW * TILES_PER_ROW) {
                val tileCol = tile % TILES_PER_ROW
                val tileRow = tile / TILES_PER_ROW
                val tileBase = tile * (TILE_DIM * TILE_DIM / 2) // 32 bytes/tile

                for (row in 0 until TILE_DIM) {
                    for (byteInRow in 0 until TILE_DIM / 2) {
                        val byte = bitmap[tileBase + row * (TILE_DIM / 2) + byteInRow].toUByte().toInt()
                        val leftIndex = byte and 0x0F
                        val rightIndex = (byte shr 4) and 0x0F

                        val x = tileCol * TILE_DIM + byteInRow * 2
                        val y = tileRow * TILE_DIM + row
                        result.setPixel(x, y, palette[leftIndex])
                        result.setPixel(x + 1, y, palette[rightIndex])
                    }
                }
            }

            return result
        }
    }
}
