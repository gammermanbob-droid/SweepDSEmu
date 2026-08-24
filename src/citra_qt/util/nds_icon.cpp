// src/citra_qt/util/nds_icon.cpp

#include "citra_qt/util/nds_icon.h"

#include <array>
#include <cstdint>
#include <fstream>

#include <QImage>

namespace NdsIcon {

namespace {

constexpr int kBannerOffsetFieldOffset = 0x68; // u32 LE, per NDS_Header.h's BannerOffset
constexpr int kBitmapOffset = 0x20;            // relative to the banner itself
constexpr int kPaletteOffset = 0x220;
constexpr int kBitmapSize = 512; // 32x32 px, 4bpp
constexpr int kPaletteSize = 32; // 16 entries, u16 each
constexpr int kIconDim = 32;
constexpr int kTileDim = 8;
constexpr int kTilesPerRow = kIconDim / kTileDim; // 4

uint32_t ReadU32LE(const std::array<uint8_t, 4>& bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

// DS colors are 5-bit-per-channel RGB555 (bit15 unused); scale each
// channel to 8 bits by replicating the top bits into the low ones
// rather than a plain *255/31, which visibly darkens saturated colors.
inline uint8_t Scale5To8(uint32_t v5) {
    return static_cast<uint8_t>((v5 << 3) | (v5 >> 2));
}

} // namespace

QPixmap Decode(const std::string& rom_path) {
    std::ifstream file(rom_path, std::ios::binary);
    if (!file) {
        return QPixmap();
    }

    file.seekg(kBannerOffsetFieldOffset, std::ios::beg);
    std::array<uint8_t, 4> offset_bytes{};
    if (!file.read(reinterpret_cast<char*>(offset_bytes.data()), offset_bytes.size())) {
        return QPixmap();
    }
    const uint32_t banner_offset = ReadU32LE(offset_bytes);
    if (banner_offset == 0) {
        return QPixmap(); // No banner in this ROM (seen in some minimal homebrew).
    }

    std::array<uint8_t, kBitmapSize> bitmap{};
    file.seekg(banner_offset + kBitmapOffset, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bitmap.data()), bitmap.size())) {
        return QPixmap();
    }

    std::array<uint8_t, kPaletteSize> palette_bytes{};
    file.seekg(banner_offset + kPaletteOffset, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(palette_bytes.data()), palette_bytes.size())) {
        return QPixmap();
    }

    // 16 palette entries, RGB555 LE. Index 0 is always transparent,
    // regardless of whatever color value happens to be stored there.
    std::array<QRgb, 16> palette{};
    for (int i = 0; i < 16; i++) {
        const uint16_t raw =
            static_cast<uint16_t>(palette_bytes[i * 2]) |
            (static_cast<uint16_t>(palette_bytes[i * 2 + 1]) << 8);
        const uint8_t r = Scale5To8(raw & 0x1F);
        const uint8_t g = Scale5To8((raw >> 5) & 0x1F);
        const uint8_t b = Scale5To8((raw >> 10) & 0x1F);
        palette[i] = (i == 0) ? qRgba(0, 0, 0, 0) : qRgba(r, g, b, 255);
    }

    QImage image(kIconDim, kIconDim, QImage::Format_ARGB32);

    // The bitmap is a 4x4 grid of 8x8 tiles; within a tile, rows run
    // top to bottom and each row's 8 pixels are packed 2-per-byte
    // (low nibble = left pixel, high nibble = right pixel).
    for (int tile = 0; tile < kTilesPerRow * kTilesPerRow; tile++) {
        const int tile_col = tile % kTilesPerRow;
        const int tile_row = tile / kTilesPerRow;
        const int tile_base = tile * (kTileDim * kTileDim / 2); // 32 bytes/tile

        for (int row = 0; row < kTileDim; row++) {
            for (int byte_in_row = 0; byte_in_row < kTileDim / 2; byte_in_row++) {
                const uint8_t byte = bitmap[tile_base + row * (kTileDim / 2) + byte_in_row];
                const uint8_t left_index = byte & 0x0F;
                const uint8_t right_index = (byte >> 4) & 0x0F;

                const int x = tile_col * kTileDim + byte_in_row * 2;
                const int y = tile_row * kTileDim + row;
                image.setPixel(x, y, palette[left_index]);
                image.setPixel(x + 1, y, palette[right_index]);
            }
        }
    }

    return QPixmap::fromImage(image);
}

} // namespace NdsIcon
