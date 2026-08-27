// src/core/core_factory.cpp

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#include "core/core_factory.h"
#include "core/melonds_core/melon_ds_core.h"

// Forward-declared: Azahar's existing 3DS core, wrapped to satisfy
// MergedCore::EmulationCore. Implement this thin adapter in
// src/core/three_ds_core_adapter.cpp — it should just forward each
// call to the existing Core::System singleton so none of Azahar's
// current 3DS emulation code has to move.
#include "core/three_ds_core_adapter.h"

namespace MergedCore {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string ExtensionOf(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    return ToLower(path.substr(dot + 1));
}

} // namespace

bool CoreFactory::LooksLikeNDS(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    // DS/DSi header logo checksum lives at 0x15C, always 0xCF56 for a
    // licensed cart (the bootrom refuses to run anything else, so
    // every dumped ROM in the wild has it).
    f.seekg(0x15C);
    uint16_t checksum = 0;
    f.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
    return f.good() && checksum == 0xCF56;
}

bool CoreFactory::LooksLike3DS(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    char magic[4] = {};

    // NCSD container (.3ds/.cci) magic at 0x100.
    f.seekg(0x100);
    f.read(magic, 4);
    if (f.good() && std::string(magic, 4) == "NCSD")
        return true;

    // Bare NCCH (.cxi/.app) magic, also at 0x100.
    f.clear();
    f.seekg(0x100);
    f.read(magic, 4);
    if (f.good() && std::string(magic, 4) == "NCCH")
        return true;

    return false;
}

SystemKind CoreFactory::Detect(const std::string& path) {
    // An empty path is MelonDSCore::Load's own convention for "boot
    // straight to the DSi Menu, no cart" (see its boot_to_menu) --
    // without this, an empty path falls through every check below to
    // the ThreeDS default, so BootDSGame("")'s DSPlayerWindow would
    // construct a ThreeDSCoreAdapter instead of the MelonDSCore it
    // actually needs.
    if (path.empty())
        return SystemKind::DS;

    const std::string ext = ExtensionOf(path);

    if (ext == "nds" || ext == "dsi" || ext == "ids")
        return SystemKind::DS;
    if (ext == "3ds" || ext == "3dsx" || ext == "cci" || ext == "cxi" ||
        ext == "cia" || ext == "app" || ext == "zcci")
        return SystemKind::ThreeDS;

    // Extension missing or unrecognized (e.g. renamed file) — sniff
    // the header. Check DS first since its signature check is
    // cheaper and has effectively zero false-positive rate.
    if (LooksLikeNDS(path))
        return SystemKind::DS;
    if (LooksLike3DS(path))
        return SystemKind::ThreeDS;

    // Default to 3DS to preserve existing Azahar behavior for files
    // this factory doesn't understand (e.g. encrypted/odd formats
    // the loader itself will reject with a clearer error).
    return SystemKind::ThreeDS;
}

std::unique_ptr<EmulationCore> CoreFactory::CreateFor(const std::string& path) {
    switch (Detect(path)) {
    case SystemKind::DS:
        return std::make_unique<MelonDSCore>();
    case SystemKind::ThreeDS:
    default:
        return std::make_unique<ThreeDSCoreAdapter>();
    }
}

} // namespace MergedCore
