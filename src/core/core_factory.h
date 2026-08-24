// include/core/core_factory.h
//
// Decides which EmulationCore to instantiate for a given file.
// This is the piece that plugs into Azahar's existing game-list /
// "boot this file" code path (src/citra_qt/main.cpp,
// GMainWindow::BootGame in the current tree) — instead of always
// constructing Core::System, that call site should go through here.

#pragma once

#include <memory>
#include <string>
#include "core/emulation_core.h"

namespace MergedCore {

class CoreFactory {
public:
    // Sniffs `path` and returns the matching core, or nullptr if the
    // file isn't recognized as either a 3DS or DS title.
    //
    // Detection order:
    //   1. Extension match (.nds/.dsi -> DS, .3ds/.3dsx/.cci/.cxi/.cia -> 3DS)
    //   2. If extension is ambiguous or missing, header sniff:
    //      - DS/DSi cartridge header has a fixed-size 0x200-byte header;
    //        offset 0x15C is the Nintendo logo checksum (0xCF56) shared
    //        by all valid DS carts — cheap and reliable.
    //      - 3DS NCSD/NCCH containers start with a "NCSD"/"NCCH" magic
    //        at offset 0x100.
    static std::unique_ptr<EmulationCore> CreateFor(const std::string& path);

    static SystemKind Detect(const std::string& path);

private:
    static bool LooksLikeNDS(const std::string& path);
    static bool LooksLike3DS(const std::string& path);
};

} // namespace MergedCore
