// src/citra_qt/util/nds_icon.h
//
// Decodes the icon embedded in an NDS ROM's header, the same way
// GameListItemPath decodes a 3DS title's SMDH icon — except DS ROMs
// carry no SMDH at all, just this much older/simpler banner format
// (see GBATEK "DS Cartridge Header - Icon/Title"). Kept independent of
// melonDS (citra_qt doesn't link melonds_core) — the handful of raw
// byte offsets this needs are a stable, well-documented part of the
// ROM header, not anything that requires pulling in the emulator core
// just to read them.

#pragma once

#include <string>

#include <QPixmap>

namespace NdsIcon {

// Returns a 32x32 QPixmap decoded from rom_path's header banner, or a
// null QPixmap if the file is too short, has no banner (BannerOffset
// == 0 — some minimal homebrew omits one), or is otherwise unreadable.
QPixmap Decode(const std::string& rom_path);

} // namespace NdsIcon
