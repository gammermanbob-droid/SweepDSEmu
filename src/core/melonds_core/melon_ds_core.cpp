// src/core/melonds_core/melon_ds_core.cpp

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "core/melonds_core/melon_ds_core.h"

#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/button_id_ds.h"
#if defined(ANDROID)
#include "common/android_utils.h"
#endif

// melonDS core headers
#include "Args.h"
#include "DSi.h"
#include "DSi_NAND.h"
#include "FreeBIOS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "SPI_Firmware.h"
#include "sha1/sha1.hpp"

namespace melonDS::Platform {
// Defined in melonds_platform_headless.cpp's SignalStop(); see the
// comment there for why this is a plain global rather than routed
// through NDS::UserData.
extern std::atomic<bool> g_console_powered_off;
} // namespace melonDS::Platform

namespace MergedCore {

namespace fs = std::filesystem;

namespace {

// Bit positions NDS::SetKeyMask expects in its 12-bit mask (bits 0-9
// feed the KEYINPUT register directly, bits 10-11 become the X/Y bits
// melonDS folds into KeyInput's bit16/17). Order matches melonDS's own
// EmuInstance::buttonNames array in frontend/qt_sdl/EmuInstanceInput.cpp
// — there is no public "Key" enum to reuse, melonDS's own frontend just
// hardcodes these positions too.
enum class DSKeyBit : int {
    A = 0,
    B = 1,
    Select = 2,
    Start = 3,
    Right = 4,
    Left = 5,
    Up = 6,
    Down = 7,
    R = 8,
    L = 9,
    X = 10,
    Y = 11,
};

std::vector<uint8_t> ReadWholeFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};

    std::streamsize size = f.tellg();
    if (size <= 0)
        return {};

    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(data.data()), size))
        return {};

    return data;
}

// Homebrew ROMs (TWiLightMenu++'s BOOT.NDS, etc.) want a real DLDI/FAT
// SD card to browse, but Azahar's sdmc folder is the user's *entire*
// 3DS library — mirroring all of it would mean importing tens or
// hundreds of GB through melonDS's FAT filesystem driver just to
// expose the handful of folders (_nds, roms) a DS homebrew menu
// actually reads. Build a small directory of symlinks into just those
// real subfolders instead, and point FATStorage's SourceDir at that —
// reads and writes still land on the real sdmc/_nds and sdmc/roms
// (see the follow_directory_symlink patch in cmake/melonds.cmake,
// required for melonDS's own directory walk to actually descend
// through these symlinks rather than treating them as empty leaves).
// FileUtil::GetUserPath() returns real absolute paths on desktop, but
// on Android it returns paths like "/sdmc/..." in a virtual convention
// that only FileUtil::'s own functions know how to resolve internally
// (AndroidUtils::TranslateFilePath, called from inside e.g.
// FileUtil::CreateDir/Exists/IOFile) -- a raw std::filesystem call on
// one of these paths, as BuildHomebrewSDCardRoot() below does
// throughout, either fails outright (no real "/sdmc" exists at the
// true filesystem root; only Android's own app sandbox can be written
// to) or, worse, is a plain no-op that looks like it succeeded. This
// is exactly why DS ROMs loaded from the game list needed the same
// translation in ds_native.cpp -- same underlying gap, different call
// site. Without it here, the homebrew SD card root this function
// builds ends up empty, and homebrew that needs real DLDI/FAT access
// (TWiLightMenu++'s BOOT.NDS, etc.) fails with "FAT init failed!".
fs::path ToRealPath(fs::path virtualPath) {
#if defined(ANDROID)
    return fs::path(AndroidUtils::TranslateFilePath(virtualPath.string()));
#else
    return virtualPath;
#endif
}

// melonDS's own SysDataDir convention (the desktop Qt frontend's own
// BIOS/firmware folder) is the primary, first-checked location -- but
// a lot of users already have DS/DSi BIOS dumps sitting wherever their
// existing SD card / homebrew setup put them (the SD root itself,
// _nds/, or a plain "bios" folder are all common conventions across
// other DS emulators and flashcart menus), and previously any of those
// being one folder off meant a silent, unexplained fall-back to
// FreeBIOS/DS-only mode instead of the real BIOS the user actually
// has. Checked in this order for every system file this function
// looks for, so wherever an existing setup already has them just
// works without needing to duplicate/move anything.
std::vector<fs::path> SystemFileSearchDirs() {
    const fs::path sysdata = ToRealPath(fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir)));
    const fs::path sdmc = ToRealPath(fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)));
    return {
        sysdata,
        sdmc,
        sdmc / "_nds",
        sdmc / "bios",
    };
}

fs::path FindSystemFile(const std::vector<fs::path>& searchDirs, const char* filename) {
    for (const fs::path& dir : searchDirs) {
        fs::path candidate = dir / filename;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

// A single bulk fs::copy(recursive) stops at the first entry it can't
// handle (a nested symlink, a permission-denied file, etc.), silently
// leaving everything after it missing -- which for `roms/` means the
// FAT/DLDI image TWiLightMenu++'s nds-bootstrap reads from can end up
// truncated, and nds-bootstrap treats that as a corrupted SD card. Walk
// the tree and copy file-by-file instead, so one bad entry doesn't take
// out the rest, and log every failure instead of swallowing it.
void CopyDirectoryRecursive(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directories(link, ec);
    if (ec) {
        LOG_ERROR(Core, "Failed to create directory {}: {}", link.string(), ec.message());
        return;
    }
    for (auto it = fs::recursive_directory_iterator(
             target, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            LOG_ERROR(Core, "Failed to walk {}: {}", target.string(), ec.message());
            ec.clear();
            continue;
        }
        const fs::path& src_path = it->path();
        const fs::path dst_path = link / fs::relative(src_path, target, ec);
        if (ec) {
            LOG_ERROR(Core, "Failed to resolve relative path for {}: {}", src_path.string(),
                       ec.message());
            ec.clear();
            continue;
        }
        if (it->is_directory(ec)) {
            fs::create_directories(dst_path, ec);
        } else {
            // recursive_directory_iterator always visits a directory
            // before its children, so dst_path's parent was already
            // created above when we visited it -- no need to redundantly
            // stat/create it again for every single file.
            fs::copy_file(src_path, dst_path, fs::copy_options::update_existing, ec);
        }
        if (ec) {
            LOG_ERROR(Core, "Failed to copy {} to {}: {}", src_path.string(), dst_path.string(),
                       ec.message());
            ec.clear();
        }
    }
}

// Some Android storage backends -- particularly FUSE-mediated scoped
// storage, which mediates access to shared/external folders an app
// doesn't have direct raw filesystem access to -- don't support
// symlink() at all; the call fails, but BuildHomebrewSDCardRoot()'s
// previous version never checked the error code, so `link` was simply
// left missing with no diagnostic. Since melonDS's own FATStorage scan
// (FATStorage::ImportDirectory) only ever sees what's actually present
// under `root`, a missing `_nds` symlink here means every file under
// it -- no matter how deep, e.g. TWiLightMenu++'s own
// _nds/TWiLightMenu/main.srldr -- is completely invisible to the
// emulated DS, regardless of whether it exists on the real device.
// Falls back to an actual recursive copy in that case; update_existing
// means this only copies new/changed files on repeat calls (this
// function runs on every homebrew boot), not a full re-copy each time.
void LinkOrCopyDirectory(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    if (fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == target) {
        return; // already a valid symlink from a previous run
    }

    // Safety net: if `link` already resolves to the exact same real
    // location as `target` -- most likely because some ANCESTOR
    // directory of `link` is itself a stale symlink pointing at an
    // ancestor of `target` (e.g. leftover from an older version of
    // this function that symlinked a whole parent directory in one
    // shot rather than per-subfolder) -- every destructive call below
    // would delete `target` itself instead of just clearing a
    // destination copy. This has actually happened: it's exactly how
    // an earlier version of this restructuring wiped a user's real
    // roms/nds, roms/gba, and roms/dsi folders on disk. Bail out
    // loudly rather than risk that again.
    if (fs::exists(link, ec)) {
        std::error_code eq_ec;
        if (fs::equivalent(target, link, eq_ec) && !eq_ec) {
            LOG_ERROR(Core,
                       "Refusing to sync {} -> {}: they already resolve to the same location on "
                       "disk (likely a stale symlink higher up) -- syncing would delete the "
                       "real source instead of just a destination copy. Leaving both untouched.",
                       target.string(), link.string());
            return;
        }
    }

    fs::remove(link, ec);
    fs::create_directory_symlink(target, link, ec);
    if (!ec && fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == target) {
        return; // symlink succeeded and actually verifies
    }

    fs::remove_all(link, ec);
    CopyDirectoryRecursive(target, link);
}

// Same idea as LinkOrCopyDirectory, for a single loose file (e.g. a
// flashcart-style auto-boot BOOT.NDS sitting directly at the real SD
// root) -- symlinked where the storage backend allows it, falling back
// to a real copy (kept in sync via update_existing semantics) otherwise.
void LinkOrCopyFile(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    if (fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == target) {
        return;
    }
    if (fs::exists(link, ec)) {
        std::error_code eq_ec;
        if (fs::equivalent(target, link, eq_ec) && !eq_ec) {
            return; // already the same file somehow -- nothing to do
        }
    }

    fs::remove(link, ec);
    fs::create_symlink(target, link, ec);
    if (!ec && fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == target) {
        return;
    }

    fs::remove(link, ec);
    fs::copy_file(target, link, fs::copy_options::update_existing, ec);
    if (ec) {
        LOG_ERROR(Core, "Failed to copy {} to {}: {}", target.string(), link.string(),
                   ec.message());
    }
}

fs::path BuildHomebrewSDCardRoot() {
    const fs::path sdmc = ToRealPath(fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)));
    // Lives inside sdmc itself (the 3DS's own SD card folder) rather than
    // as a separate sibling directory, so there's one SD card location to
    // browse/manage instead of two -- excluded from the top-level mirror
    // loop below the same way "roms" and "Nintendo 3DS" are, since it's
    // now nested inside the very tree that loop walks.
    const fs::path root = sdmc / "nds_sdcard_root";

    // Now that real DSi system files make Load() construct a genuine
    // melonDS::DSi for every DS boot (not just homebrew ones -- a real
    // DSi's SD card is a system-level resource, live regardless of what
    // cart is inserted), this function runs on every single DS game
    // load, retail carts included. The mirror sync below is idempotent
    // -- re-running it against an already-up-to-date tree just repeats
    // the same stat() calls with nothing to actually copy -- but each of
    // those stat() calls still pays Android's FUSE-mediated scoped-
    // storage round-trip cost, and a homebrew library with a large media
    // collection (MoonShell2 movies, comics, ...) can mean thousands of
    // them. Skip the whole walk after the first successful run each
    // process lifetime; a user adding new homebrew files mid-session
    // just needs a full app restart to pick them up, same tradeoff
    // EnsureSDCardImageIsFresh's own session cache already makes.
    static bool s_built_this_session = false;
    if (s_built_this_session) {
        return root;
    }

    std::error_code ec;
    fs::create_directories(root, ec);

    // Mirror every top-level entry directly under sdmc except "roms"
    // (handled separately below, per DS/DSi/GBA platform subfolder only)
    // and "Nintendo 3DS" (the emulated 3DS's own title/save/extdata
    // library, which can genuinely run into the tens or hundreds of GB
    // -- see this function's own original design note below). There's
    // no fixed list of folder/file names that covers every homebrew
    // menu or utility's own particular needs (TWiLightMenu++'s _nds,
    // a flashcart-style auto-boot BOOT.NDS sitting loose at the SD
    // root, MoonShell2/ComicBookDS's own resource folders, ...) --
    // previously only a hardcoded handful of names were synced at all,
    // so anything outside that list (confirmed here: a user's _nds
    // folder simply didn't exist yet because nothing had ever created
    // it) was invisible to the emulated DS regardless of what the real
    // sdmc folder actually had. LinkOrCopyDirectory/LinkOrCopyFile
    // prefer a real symlink wherever the storage backend supports one,
    // so this costs no extra disk space in the common case -- only
    // Android's FUSE-mediated scoped storage (which can't symlink at
    // all) pays for an actual copy, exactly like the old allowlist's
    // own entries already did.
    // Unambiguously 3DS-only -- never something a DS homebrew menu reads,
    // but easy to end up sitting at sdmc's top level next to genuinely
    // DS-relevant content (a user's own CIA installs, 3DS-only companion
    // homebrew folders). Mirroring these in only bloats the DS's FAT
    // image for no benefit: melonDS's FATStorage does uncached,
    // one-syscall-per-512-byte-sector file I/O with no read-ahead, so an
    // image bloated well past available RAM turns into real random disk
    // seeks for anything that streams through it at speed (MoonShell2
    // video/audio playback in particular).
    static constexpr std::array<std::string_view, 3> kThreeDsOnlyNames = {
        "3ds cias",
        "Mp3 4 3DS",
        "CTGP-7",
    };
    for (const auto& entry : fs::directory_iterator(sdmc, ec)) {
        const fs::path& name = entry.path().filename();
        const std::string name_str = name.string();
        std::string ext_str = name.extension().string();
        std::transform(ext_str.begin(), ext_str.end(), ext_str.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        // "nds_sdcard_root" is this very function's own output directory,
        // now nested inside sdmc -- and the "nds_sdcard.img"/
        // "dsi_sdcard.img" FAT images (plus their .idx/.contents
        // sidecars) live here too now, neither of which are homebrew
        // content to mirror into themselves.
        if (name == "roms" || name == "Nintendo 3DS" || name == "nds_sdcard_root" ||
            name_str.starts_with("nds_sdcard.img") || name_str.starts_with("dsi_sdcard.img") ||
            ext_str == ".cia" || ext_str == ".cxi" || ext_str == ".3dsx" ||
            std::find(kThreeDsOnlyNames.begin(), kThreeDsOnlyNames.end(), name_str) !=
                kThreeDsOnlyNames.end()) {
            continue;
        }
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            LinkOrCopyDirectory(entry.path(), root / name);
        } else if (entry.is_regular_file(entry_ec)) {
            LinkOrCopyFile(entry.path(), root / name);
        }
    }

    // Only sync the DS/DSi/GBA subfolders nds-bootstrap and GBARunner2
    // actually read from. roms/ commonly also holds entirely unrelated
    // platforms alongside them (e.g. roms/psx, for a separate PS1
    // core) -- syncing those too just wastes space and time in this
    // DS-only virtual SD card, and can push its content past the FAT
    // image's capacity, silently dropping unrelated files (see
    // EnsureSDCardImageIsFresh).
    // A previous version of this function symlinked the whole of
    // `roms` in one shot (root/roms -> sdmc/roms) rather than per
    // platform subfolder as below. If that symlink is still here from
    // an older run, `root / "roms" / platform` would resolve straight
    // through it to `sdmc / "roms" / platform` -- the exact same real
    // directory LinkOrCopyDirectory is about to treat as a "link" to
    // manage, which is what let it delete real user data (see
    // LinkOrCopyDirectory's own equivalence check, now a second layer
    // of protection against this same mistake). fs::remove() only
    // removes the symlink itself, never anything it points to.
    if (fs::is_symlink(root / "roms", ec)) {
        fs::remove(root / "roms", ec);
    }
    fs::create_directories(root / "roms", ec);
    for (const char* platform : {"nds", "dsi", "gba"}) {
        const fs::path target = sdmc / "roms" / platform;
        if (!fs::is_directory(target, ec))
            continue;
        LinkOrCopyDirectory(target, root / "roms" / platform);
    }

    // MoonShell2 specifically insists its own "moonshl2" resource
    // folder sit at the true SD card root — separately from wherever
    // its .nds launcher itself lives — and refuses to start otherwise
    // ("NDS file version... and installed moonshl2 folder... are
    // different... it is impossible when arranging it besides the
    // root of SD card", straight from its own on-screen error). Real
    // hardware has this exact same requirement; this user's copy has
    // it nested under roms/nds/ instead, alongside the .nds files, so
    // alias it up to root without moving/duplicating anything real.
    {
        const fs::path moonshell_target = sdmc / "roms" / "nds" / "moonshl2";
        if (fs::is_directory(moonshell_target, ec)) {
            LinkOrCopyDirectory(moonshell_target, root / "moonshl2");
        }
    }

    s_built_this_session = true;
    return root;
}

// melonDS's own FATStorage::ImportDirectory (see FATStorage.cpp) already
// re-syncs incrementally on every single construction -- comparing each
// file's size/mtime against its own persisted index and only re-importing
// what actually changed -- so it does NOT need our help noticing ordinary
// added/changed files; an earlier version of this comment claimed
// otherwise and was wrong. The one thing FATStorage genuinely can't fix on
// its own is its *size*: that's decided once, at first creation, from
// sourceDir's size at that moment, and never revisited -- if sourceDir
// later outgrows it, ImportDirectory silently drops whatever doesn't fit,
// with no error beyond whatever the homebrew itself reports (e.g.
// TWiLightMenu++'s own "no SD card inserted" at boot). This exists purely
// to catch *that* one case and force a resize by deleting the image so
// FATStorage recreates it at a size that fits.
//
// Walking all of sourceDir to total its size is real I/O, though --
// cheap for a small homebrew folder, but this project's own sourceDir can
// run into the tens of GB (MoonShell2 movie files, a large ROM
// collection, ...), and repeating that walk on every single DS game
// launch was measurably slowing down load times even for retail games
// that never touch the SD card at all. sourceDir only actually changes
// when the user manually drops new files into it between sessions, so
// checking once per process lifetime per image path is enough --
// s_checked_this_session below is intentionally never cleared.
void EnsureSDCardImageIsFresh(const fs::path& imgPath, const fs::path& sourceDir) {
    static std::set<std::string> s_checked_this_session;
    if (!s_checked_this_session.insert(imgPath.string()).second) {
        return; // already verified this image once this process run
    }

    std::error_code ec;
    if (!fs::exists(imgPath, ec)) {
        return; // no existing image yet; FATStorage will import sourceDir fresh
    }

    std::uintmax_t contentSize = 0;
    for (auto it = fs::recursive_directory_iterator(
             sourceDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_directory(ec)) {
            continue;
        }
        contentSize += it->file_size(ec);
    }

    // Match FATStorage's own 128MB leeway so a rebuild isn't triggered by
    // the same margin it would already account for on first creation.
    constexpr std::uintmax_t kLeeway = 0x8000000ULL;
    const auto imgSize = fs::file_size(imgPath, ec);
    if (!ec && contentSize + kLeeway > imgSize) {
        LOG_WARNING(Core,
                    "SD card image {} ({} bytes) is too small for its current source content "
                    "({} bytes) -- deleting so it gets rebuilt at the correct size",
                    imgPath.string(), imgSize, contentSize);
        fs::remove(imgPath, ec);
        fs::remove(fs::path(imgPath.string() + ".idx"), ec);
    }
}

template <size_t N>
bool ReadFileExact(const fs::path& path, std::array<uint8_t, N>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(N));
    return static_cast<size_t>(f.gcount()) == N;
}

} // namespace

MelonDSCore::MelonDSCore() = default;
MelonDSCore::~MelonDSCore() { Shutdown(); }

// See this function's own declaration in melon_ds_core.h for the summary.
// Longer version: DSiWare is fundamentally designed to run from
// NAND-installed content, not as a loose ROM file -- direct-booting a
// bare .nds/.dsi DSiWare dump (MelonDSCore::Load's normal, boot_to_menu
// == false path) is fragile even in upstream melonDS, since a title's own
// code expects NAND-resident save/config metadata that direct-boot's
// emulated setup can't fully fake for every title (see melonDS issues
// #1577 and #2623 upstream). This uses melonDS's own
// DSi_NAND::NANDMount::ImportTitle -- the same primitive melonDS's own
// Title Manager dialog calls -- with a Title Metadata (TMD) synthesized
// from the ROM's own DSi-extended header fields (DSiTitleIDHigh/Low),
// matching DS-Homebrew/NTM's maketmd.c byte-for-byte (github.com/
// Epicpkmn11/NTM) -- a real DSi homebrew tool proven to produce TMDs
// real hardware accepts for titles with no genuine Nintendo-issued TMD,
// which is exactly our situation here.
//
// This writes a fully correct, verified entry -- confirmed byte-for-byte
// (via a standalone NAND-crypto/FAT decoder built for this investigation)
// to persist on NAND exactly as melonDS's own Title Manager would produce
// it. It does NOT reliably survive being displayed on the DSi Menu: the
// real Menu firmware, running for real inside melonDS's own CPU emulation,
// deletes the entry during its own boot sequence sometime after this
// function returns -- confirmed by decrypting the exact NAND bytes
// immediately after install (entry present, attribute 0x10) and again
// immediately after one Menu boot (entry gone, FAT deletion marker 0xE5
// written over it). Real, unsigned NAND-injected DSiWare is well known to
// work fine on genuine DSi hardware (this is the entire basis of tools
// like NTM), so this is a melonDS-side gap in emulating whatever
// validation the real System Menu performs on titles it finds -- not
// something fixable from this side. Kept as-is (rather than reverted)
// since the NAND write itself is correct and harmless, and may start
// working unmodified if melonDS fixes this upstream.
std::string InstallDSiWareTitleToNAND(const std::string& rom_path) {
    const std::vector<fs::path> searchDirs = SystemFileSearchDirs();
    fs::path dsi_bios7_path = FindSystemFile(searchDirs, "dsi_bios7.bin");
    fs::path dsi_nand_path = FindSystemFile(searchDirs, "dsi_nand.bin");
    if (dsi_bios7_path.empty() || dsi_nand_path.empty()) {
        return "DSi system files (dsi_bios7.bin / dsi_nand.bin) not found.";
    }

    melonDS::DSiBIOSImage bios7i_data{};
    if (!ReadFileExact(dsi_bios7_path, bios7i_data)) {
        return "Failed to read dsi_bios7.bin.";
    }

    std::vector<uint8_t> romdata = ReadWholeFile(fs::path(rom_path));
    if (romdata.size() < sizeof(melonDS::NDSHeader)) {
        return "ROM file is missing or too small to be a valid DS/DSi ROM.";
    }

    melonDS::NDSHeader header{};
    std::memcpy(&header, romdata.data(), sizeof(header));
    if (!header.IsDSi()) {
        return "This ROM has no DSi-exclusive title data to install.";
    }

    SHA1_CTX sha_ctx;
    SHA1Init(&sha_ctx);
    SHA1Update(&sha_ctx, romdata.data(), static_cast<uint32_t>(romdata.size()));
    std::array<uint8_t, 20> content_hash{};
    SHA1Final(content_hash.data(), &sha_ctx);

    const auto put_be32 = [](uint8_t* dst, uint32_t v) {
        dst[0] = static_cast<uint8_t>(v >> 24);
        dst[1] = static_cast<uint8_t>(v >> 16);
        dst[2] = static_cast<uint8_t>(v >> 8);
        dst[3] = static_cast<uint8_t>(v);
    };

    // Field choices here deliberately mirror DS-Homebrew/NTM's maketmd.c
    // (github.com/Epicpkmn11/NTM) byte-for-byte -- a real DSi homebrew
    // tool that's known to produce TMDs real hardware actually accepts
    // and displays for titles with no genuine Nintendo-issued TMD, which
    // is exactly our situation. In particular: NTM leaves SignatureType/
    // SignatureName/PublicSaveSize/PrivateSaveSize all zero (an earlier
    // version of this function filled those in, which is one candidate
    // for why the title imported cleanly at the FAT level but never
    // appeared on the Menu) and -- the field that stood out most --
    // fills all 16 AgeRatings bytes with 0x80, not zero.
    melonDS::DSi_TMD::TitleMetadata tmd{};
    tmd.GroupId[0] = static_cast<uint8_t>(header.MakerCode[0]);
    tmd.GroupId[1] = static_cast<uint8_t>(header.MakerCode[1]);
    std::memset(tmd.AgeRatings, 0x80, sizeof(tmd.AgeRatings));

    put_be32(&tmd.TitleId[0], header.DSiTitleIDHigh);
    put_be32(&tmd.TitleId[4], header.DSiTitleIDLow);

    tmd.NumberOfContents = 1;

    tmd.Contents.ContentId[0] = tmd.Contents.ContentId[1] = 0;
    tmd.Contents.ContentId[2] = tmd.Contents.ContentId[3] = 0;
    tmd.Contents.ContentIndex[0] = tmd.Contents.ContentIndex[1] = 0;
    tmd.Contents.ContentType[0] = 0x00;
    tmd.Contents.ContentType[1] = 0x01;
    for (int i = 0; i < 8; i++) {
        tmd.Contents.ContentSize[i] =
            static_cast<uint8_t>(static_cast<uint64_t>(romdata.size()) >> (8 * (7 - i)));
    }
    std::memcpy(tmd.Contents.ContentSha1Hash, content_hash.data(), 20);

    auto* nand_file = melonDS::Platform::OpenLocalFile(
        dsi_nand_path.string(), melonDS::Platform::FileMode::ReadWriteExisting);
    if (!nand_file) {
        return "Failed to open dsi_nand.bin for writing.";
    }

    melonDS::DSi_NAND::NANDImage nand(nand_file, &bios7i_data[0x8308]);
    if (!nand) {
        return "dsi_nand.bin could not be read as a valid DSi NAND image.";
    }

    melonDS::DSi_NAND::NANDMount mount(nand);
    if (!mount) {
        return "Failed to mount the DSi NAND's filesystem.";
    }

    // Re-installing (e.g. after fixing a bug in this installer) should
    // cleanly replace whatever's already there rather than mixing old
    // and new content under the same title ID.
    if (mount.TitleExists(tmd.GetCategory(), tmd.GetID())) {
        mount.DeleteTitle(tmd.GetCategory(), tmd.GetID());
    }

    if (!mount.ImportTitle(romdata.data(), romdata.size(), tmd, /*readonly=*/false)) {
        return "melonDS's NAND importer rejected this title.";
    }

    return {};
}

std::string MelonDSCore::SaveDirFor(const std::string& rom_path) const {
    // Azahar's existing save layout: <user_data>/sdmc/... for 3DS.
    // Route DS saves to a sibling directory instead of anywhere under
    // that tree so the two systems' save files can never shadow one
    // another, even if a 3DS and DS title happened to share a name.
    fs::path rom(rom_path);
    fs::path dir = ToRealPath(fs::path(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) / "nds_saves");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / rom.stem()).string() + ".sav";
}

ResultStatus MelonDSCore::Load(Frontend::EmuWindow& /*window*/, const std::string& path) {
    // An empty path means "boot straight to the DSi Menu, no cart" --
    // see the boot_to_menu branch further down. Real hardware does
    // exactly this when powered on with no cart inserted; a real DSi
    // NAND is required since FreeBIOS's synthesized firmware has no
    // menu at all to boot into (checked once real DSi files are looked
    // for below, since that's also where NeedsDirectBoot()-worthy real
    // system files get discovered either way).
    const bool boot_to_menu = path.empty();
    if (!boot_to_menu && !fs::exists(path)) {
        return ResultStatus::ErrorLoader;
    }

    // Discard any stale power-off signal left over from a previous
    // session — g_console_powered_off is a single global (see its
    // definition), not scoped to a particular NDS instance.
    melonDS::Platform::g_console_powered_off.store(false, std::memory_order_relaxed);

    // BuildHomebrewSDCardRoot() walks and syncs the user's whole roms/
    // _nds/moonshl2 tree on every call — expensive on Android's
    // FUSE-mediated storage with a sizeable ROM library. The DSi-NAND
    // path below and the NDS-cart path further down both used to call
    // it independently (identical work, done twice, every single
    // boot); cache it here so it only actually runs once per Load().
    std::optional<std::string> cached_homebrew_sdcard_root;
    auto get_homebrew_sdcard_root = [&cached_homebrew_sdcard_root]() -> const std::string& {
        if (!cached_homebrew_sdcard_root) {
            cached_homebrew_sdcard_root = BuildHomebrewSDCardRoot().string();
        }
        return *cached_homebrew_sdcard_root;
    };

    melonDS::NDSArgs args{};

    // FastMem backs JIT-compiled code's direct memory accesses with a
    // SIGSEGV/SIGBUS-driven fixup scheme (see the ARMJIT_Memory patch
    // in cmake/melonds.cmake for the crash that scheme was hitting in
    // this embedding). Even with that crash fixed, catching a real
    // signal on every cold memory access is inherently fragile
    // alongside Azahar's own dynarmic JIT in the same process — keep
    // JIT-compiled CPU execution (still fast) but route memory
    // accesses through regular bounds-checked reads/writes instead of
    // the signal-handler fast path.
    //
    // (An earlier version of this also fully disabled the JIT on
    // Android, on the theory that JIT-compiled-code execution itself
    // was crashing there. It wasn't: the actual crash was
    // ARMJIT_Memory's constructor failing to set up its fastmem arena
    // at all on Android, unconditionally, before any JIT code ever
    // ran -- see melonds_platform_headless.cpp's DynamicLibrary_Load
    // for the real fix. JIT is exactly as safe on Android as anywhere
    // else once that arena actually initializes correctly.)
    if (args.JIT) {
        args.JIT->FastMemory = false;
    }

    // melonDS can run with a real dumped BIOS/firmware or with its
    // built-in FreeBIOS + a synthesized firmware. Prefer real
    // system files if the user has already provided them for
    // melonDS standalone (reuse melonDS's own config path so users
    // don't have to redump anything just because they're now
    // launching DS titles from inside Azahar); fall back to FreeBIOS
    // otherwise so a DS ROM still boots with zero extra setup, matching
    // the "just works" experience Azahar aims for with 3DS titles.
    // NDSArgs already defaults ARM9BIOS/ARM7BIOS/Firmware to
    // FreeBIOS + generated firmware, so the fallback case needs no
    // extra code here — just leave `args` alone. See SystemFileSearchDirs()
    // for why this checks more than just melonDS's own SysDataDir.
    const std::vector<fs::path> searchDirs = SystemFileSearchDirs();
    fs::path bios9 = FindSystemFile(searchDirs, "bios9.bin");
    fs::path bios7 = FindSystemFile(searchDirs, "bios7.bin");
    fs::path firmware = FindSystemFile(searchDirs, "firmware.bin");

    if (!bios9.empty() && !bios7.empty() && !firmware.empty()) {
        melonDS::ARM9BIOSImage bios9data{};
        melonDS::ARM7BIOSImage bios7data{};
        std::vector<uint8_t> firmwaredata = ReadWholeFile(firmware);

        if (ReadFileExact(bios9, bios9data) && ReadFileExact(bios7, bios7data) &&
            !firmwaredata.empty()) {
            args.ARM9BIOS = std::make_unique<melonDS::ARM9BIOSImage>(bios9data);
            args.ARM7BIOS = std::make_unique<melonDS::ARM7BIOSImage>(bios7data);
            args.Firmware = melonDS::Firmware(firmwaredata.data(),
                                               static_cast<melonDS::u32>(firmwaredata.size()));
        }
        // else: one of the dumps is malformed. Fall through and keep
        // NDSArgs's FreeBIOS/generated-firmware defaults rather than
        // mixing a real BIOS with generated firmware or vice versa.
    }

    // DSi mode needs the DS-compat BIOS/firmware above *plus* real DSi
    // ARM9i/ARM7i BIOS dumps and a NAND image — a real DSi (and a 3DS
    // running DS software in backwards-compatible/TWiLightMenu-style
    // setups) accesses its SD card as genuine DSi SDIO hardware, not
    // through the NDSCart-level homebrew/DLDI trick below. Prefer this
    // over plain DS mode whenever all four DSi-side files are present;
    // fall through to plain NDS otherwise.
    std::unique_ptr<melonDS::DSiBIOSImage> dsi_bios9;
    std::unique_ptr<melonDS::DSiBIOSImage> dsi_bios7;
    std::vector<uint8_t> dsi_firmwaredata;
    std::optional<melonDS::DSi_NAND::NANDImage> dsi_nand;

    if (!bios9.empty() && !bios7.empty() && !firmware.empty()) {
        fs::path dsi_bios9_path = FindSystemFile(searchDirs, "dsi_bios9.bin");
        fs::path dsi_bios7_path = FindSystemFile(searchDirs, "dsi_bios7.bin");
        fs::path dsi_firmware_path = FindSystemFile(searchDirs, "dsi_firmware.bin");
        fs::path dsi_nand_path = FindSystemFile(searchDirs, "dsi_nand.bin");

        if (!dsi_bios9_path.empty() && !dsi_bios7_path.empty() &&
            !dsi_firmware_path.empty() && !dsi_nand_path.empty()) {
            melonDS::DSiBIOSImage bios9i_data{};
            melonDS::DSiBIOSImage bios7i_data{};
            dsi_firmwaredata = ReadWholeFile(dsi_firmware_path);

            if (ReadFileExact(dsi_bios9_path, bios9i_data) &&
                ReadFileExact(dsi_bios7_path, bios7i_data) && !dsi_firmwaredata.empty()) {
                // The NAND's ES keyY material lives at a fixed offset
                // inside the real ARM7i BIOS — melonDS's own frontend
                // reads it from the exact same spot (EmuInstance::loadNAND).
                auto* nand_file = melonDS::Platform::OpenLocalFile(
                    dsi_nand_path.string(), melonDS::Platform::FileMode::ReadWriteExisting);
                if (nand_file) {
                    melonDS::DSi_NAND::NANDImage candidate(nand_file, &bios7i_data[0x8308]);
                    if (candidate) {
                        dsi_nand = std::move(candidate);
                        dsi_bios9 = std::make_unique<melonDS::DSiBIOSImage>(bios9i_data);
                        dsi_bios7 = std::make_unique<melonDS::DSiBIOSImage>(bios7i_data);
                    }
                    // else: OpenLocalFile succeeded but the NAND itself
                    // is malformed — candidate's destructor closes the
                    // file handle it took ownership of either way.
                }
            }
        }
    }

    // dsi_nand is moved-from below once construction commits to the DSi
    // branch -- capture whether we actually got a real DSi NAND before
    // that happens, since boot_to_menu needs to know afterward and can't
    // ask dsi_nand itself anymore by then.
    const bool constructed_dsi = dsi_nand.has_value();

    if (dsi_nand.has_value()) {
        // Lives inside sdmc (the 3DS's own SD card folder) alongside
        // nds_sdcard_root -- see BuildHomebrewSDCardRoot's doc comment.
        const fs::path dsi_sdcard_img = ToRealPath(
            fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)) / "dsi_sdcard.img");
        EnsureSDCardImageIsFresh(dsi_sdcard_img, fs::path(get_homebrew_sdcard_root()));
        melonDS::DSiArgs dsi_args{
            std::move(args),
            std::move(dsi_bios9),
            std::move(dsi_bios7),
            std::move(dsi_nand),
            std::make_optional<melonDS::FATStorage>(melonDS::FATStorageArgs{
                dsi_sdcard_img.string(),
                0, // auto-size from the source directory's contents
                false,
                get_homebrew_sdcard_root(),
            }),
            false, // DSPHLE — the DSP is emulated separately from Teak DSi_DSP; leave off for now
        };
        dsi_args.Firmware = melonDS::Firmware(dsi_firmwaredata.data(),
                                              static_cast<melonDS::u32>(dsi_firmwaredata.size()));
        nds_ = std::make_unique<melonDS::DSi>(std::move(dsi_args));
    } else {
        nds_ = std::make_unique<melonDS::NDS>(std::move(args));
    }

    // melonDS's own frontend resets twice: once right after construction
    // with no cart in the slot (establishing a clean hardware baseline —
    // this is what a real console's own power-on self-test sees), then
    // again after SetNDSCart() below. Skipping this first, cart-less
    // reset and going straight to "construct, then insert+reset once"
    // (which is what we used to do) leaves cart-slot detection state
    // from whatever the constructor happened to default to, rather than
    // a state that was actually established by a real reset — harmless
    // for FreeBIOS's own direct-boot shortcut, but with a real BIOS/NAND
    // now wired in, the DSi Menu's own (Nintendo-authored, not ours)
    // cart-detection logic runs as real code during boot and needs that
    // baseline to correctly notice a cart appearing afterward.
    nds_->Reset();

    if (boot_to_menu) {
        if (!constructed_dsi) {
            // No real DSi NAND -- FreeBIOS's synthesized firmware has no
            // menu at all to boot into, only its own direct-boot shortcut.
            nds_.reset();
            return ResultStatus::ErrorSystemMode;
        }
        // No cart to insert: the baseline Reset() above plus this Start()
        // is the whole boot sequence -- exactly what a real DSi's own
        // bootrom/firmware does when powered on with no cart present,
        // landing on the DSi Menu (its own, Nintendo-authored code, not
        // ours) same as real hardware.
        nds_->Start();
        loaded_rom_path_.clear();
        loaded_ = true;
        return ResultStatus::Success;
    }

    std::vector<uint8_t> romdata = ReadWholeFile(path);
    if (romdata.empty()) {
        nds_.reset();
        return ResultStatus::ErrorLoader;
    }

    auto rombuffer = std::make_unique<melonDS::u8[]>(romdata.size());
    std::memcpy(rombuffer.get(), romdata.data(), romdata.size());

    // Homebrew ROMs (TWiLightMenu++'s BOOT.NDS, etc.) expect real DLDI/FAT
    // SD card access — without it they print "FAT init failed!" and stop.
    // melonDS already has this built in (NDSCart::ParseROM auto-detects
    // homebrew via the header and only consults SDCard in that case, so
    // this is a no-op for ordinary retail carts): back it with a
    // persistent FAT image seeded from — and synced back to — just the
    // real subfolders a DS homebrew menu actually needs (see
    // BuildHomebrewSDCardRoot()), not the user's whole 3DS library.
    //
    // Retail carts never consult SDCard at all (see the ParseROM
    // comment above), so building/syncing it for them was pure waste —
    // skip it entirely unless the ROM's own header says it's homebrew,
    // rather than paying for a full roms/ tree sync on every single
    // retail game boot.
    melonDS::NDSCart::NDSCartArgs cart_args{};
    bool rom_is_homebrew = false;
    if (romdata.size() >= sizeof(melonDS::NDSHeader)) {
        melonDS::NDSHeader header{};
        std::memcpy(&header, romdata.data(), sizeof(header));
        rom_is_homebrew = header.IsHomebrew();
    }
    if (rom_is_homebrew) {
        // Lives inside sdmc (the 3DS's own SD card folder) alongside
        // nds_sdcard_root -- see BuildHomebrewSDCardRoot's doc comment.
        const fs::path nds_sdcard_img = ToRealPath(
            fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)) / "nds_sdcard.img");
        EnsureSDCardImageIsFresh(nds_sdcard_img, fs::path(get_homebrew_sdcard_root()));
        cart_args.SDCard = melonDS::FATStorageArgs{
            nds_sdcard_img.string(),
            0, // auto-size from the source directory's contents
            false,
            get_homebrew_sdcard_root(),
        };
    }

    auto cart = melonDS::NDSCart::ParseROM(
        std::move(rombuffer), static_cast<melonDS::u32>(romdata.size()), nullptr,
        std::move(cart_args));
    if (!cart) {
        nds_.reset();
        return ResultStatus::ErrorLoader_ErrorInvalidFormat;
    }

    // Cart must be inserted before Reset() — Reset() establishes the
    // CPU's initial state from whatever's currently in the slot, and
    // SetupDirectBoot() below reads the cart's header/ROM data
    // directly, so both need SetNDSCart() to have already happened.
    nds_->SetNDSCart(std::move(cart));
    nds_->Reset();

    // Always direct-boot straight into the cart's entrypoint, matching
    // Azahar's own "pick a game, it just boots" UX for 3DS titles
    // rather than sitting at a firmware/system menu first — melonDS's
    // own frontend offers this as a user-configurable "Direct Boot"
    // option independent of NeedsDirectBoot() (which is unconditionally
    // true for FreeBIOS anyway, but now false once real BIOS/NAND is
    // present per the blocks above, which would otherwise boot into
    // the DS firmware settings screen or the real DSi Menu instead of
    // straight into whatever ROM was actually requested — including
    // BOOT.NDS itself, which isn't designed to be picked as an icon
    // from the DSi Menu in the first place; real flashcarts/CFW setups
    // boot straight into it exactly like this).
    nds_->SetupDirectBoot(fs::path(path).filename().string());

    // Reset() alone leaves the console halted — NDS::RunFrame()'s main
    // loop is gated on the Running flag, which only Start() sets. Every
    // prior RunFrame() call was silently a no-op without this: frames
    // "completed" instantly (no CPU cycles executed) and the GPU never
    // produced anything but a blank framebuffer, which read as a
    // convincing but misleading "it boots but shows a black screen."
    nds_->Start();

    // Point save data at our own directory (see SaveDirFor) rather than
    // melonDS's default, which assumes it's the only emulator touching
    // the filesystem. A legacy save sitting next to the ROM itself --
    // the convention real flashcarts, DraStic, and melonDS's own
    // desktop frontend all use by default -- is synced in on every
    // load whenever it's newer than what's in our own directory, not
    // just the first time: some ROM tools/companion apps write or
    // refresh that adjacent .sav on their own at any point, not only
    // before a game's first launch here, so a one-time check would
    // miss anything created or updated later. Comparing mtimes (rather
    // than always preferring the legacy file) is what keeps this from
    // clobbering a newer save this core itself already wrote back.
    const std::string save_path = SaveDirFor(path);
    std::error_code ec;
    fs::path legacy_save = fs::path(path);
    legacy_save.replace_extension(".sav");
    if (fs::exists(legacy_save, ec)) {
        bool sync_from_legacy = !fs::exists(save_path, ec);
        if (!sync_from_legacy) {
            const auto legacy_time = fs::last_write_time(legacy_save, ec);
            const auto our_time = fs::last_write_time(save_path, ec);
            sync_from_legacy = !ec && legacy_time > our_time;
        }
        if (sync_from_legacy) {
            fs::copy_file(legacy_save, save_path, fs::copy_options::overwrite_existing, ec);
        }
    }
    std::vector<uint8_t> savedata = ReadWholeFile(save_path);
    if (!savedata.empty()) {
        nds_->SetNDSSave(savedata.data(), static_cast<melonDS::u32>(savedata.size()));
    }

    loaded_rom_path_ = path;
    loaded_ = true;
    return ResultStatus::Success;
}

void MelonDSCore::ApplyInput(const InputState& input) {
    if (!nds_)
        return;

    // melonDS's key mask uses a "clear bit means pressed" inverted
    // logic (matches real DS hardware registers) — invert while
    // translating.
    uint32_t melon_keymask = 0xFFFFFFFFu;

    auto set = [&](bool pressed, DSKeyBit bit) {
        if (pressed)
            melon_keymask &= ~(1u << static_cast<int>(bit));
    };

    set(input.buttons & DS_BTN_A, DSKeyBit::A);
    set(input.buttons & DS_BTN_B, DSKeyBit::B);
    set(input.buttons & DS_BTN_X, DSKeyBit::X);
    set(input.buttons & DS_BTN_Y, DSKeyBit::Y);
    set(input.buttons & DS_BTN_L, DSKeyBit::L);
    set(input.buttons & DS_BTN_R, DSKeyBit::R);
    set(input.buttons & DS_BTN_START, DSKeyBit::Start);
    set(input.buttons & DS_BTN_SELECT, DSKeyBit::Select);
    set(input.buttons & DS_BTN_UP, DSKeyBit::Up);
    set(input.buttons & DS_BTN_DOWN, DSKeyBit::Down);
    set(input.buttons & DS_BTN_LEFT, DSKeyBit::Left);
    set(input.buttons & DS_BTN_RIGHT, DSKeyBit::Right);

    nds_->SetKeyMask(melon_keymask);

    if (input.touch_pressed) {
        nds_->TouchScreen(input.touch_x, input.touch_y);
    } else {
        nds_->ReleaseScreen();
    }
}

void MelonDSCore::SplitFramebuffer(FrameOutput& out) {
    if (!nds_)
        return;

    // melonDS's GPU exposes the two screens as separate RAM
    // framebuffers already (GPU::GetFramebuffers), each 256x192 —
    // no splitting of a combined buffer needed.
    void* top_ptr = nullptr;
    void* bottom_ptr = nullptr;
    if (!nds_->GPU.GetFramebuffers(&top_ptr, &bottom_ptr) || !top_ptr || !bottom_ptr)
        return;

    const uint32_t* top = static_cast<const uint32_t*>(top_ptr);
    const uint32_t* bottom = static_cast<const uint32_t*>(bottom_ptr);

    out.top.width = kScreenWidth;
    out.top.height = kScreenHeight;
    out.top.pixels.assign(top, top + (kScreenWidth * kScreenHeight));

    out.bottom.width = kScreenWidth;
    out.bottom.height = kScreenHeight;
    out.bottom.pixels.assign(bottom, bottom + (kScreenWidth * kScreenHeight));

    // No ScreenSwap handling needed here: SoftRenderer's own per-
    // scanline composite (GPU_Soft.cpp) already picks which of its two
    // Framebuffer slots each hardware engine writes into based on
    // GPU.ScreenSwap, so GetFramebuffers()'s "top"/"bottom" already
    // reflect the correct physical LCD assignment. Swapping them again
    // here undid that and put the touch-capable screen's content in
    // the top half of the window while touch input (see UpdateTouch()
    // in ds_player_window.cpp) still only ever accepted clicks in the
    // bottom half — this is what looked like "the screens are swapped
    // and I can't click the on-screen keyboard."
}

void MelonDSCore::ReadAudio(FrameOutput& out) {
    const int available = nds_->SPU.GetOutputSize();
    if (available <= 0)
        return;

    out.audio_samples.resize(static_cast<size_t>(available) * 2);
    const int got = nds_->SPU.ReadOutput(out.audio_samples.data(), available);
    out.audio_samples.resize(static_cast<size_t>(got) * 2);
}

void MelonDSCore::RunFrame(const InputState& input, FrameOutput& out) {
    if (!loaded_ || !nds_)
        return;

    ApplyInput(input);
    nds_->RunFrame();
    SplitFramebuffer(out);
    ReadAudio(out);

    // See the g_console_powered_off comment above — once this fires,
    // nds_->RunFrame() is a permanent no-op from here on, so surface it
    // and let the caller stop asking for more frames.
    out.powered_off =
        melonDS::Platform::g_console_powered_off.exchange(false, std::memory_order_relaxed);
}

void MelonDSCore::Reset() {
    if (!nds_) {
        return;
    }
    // A bare nds_->Reset() resets hardware state and leaves the system to
    // boot normally from there -- fine with FreeBIOS's own direct-boot
    // shortcut, but now that real DSi system files make this a genuine
    // melonDS::DSi with a real NAND, "normally" means booting into the
    // actual DSi Menu, not back into whatever game was running (exactly
    // what a real console's own hardware reset does too). "Reset Game"
    // is meant to restart the *current* game from power-on, so redo the
    // same DirectBoot injection Load() does after its own Reset() --
    // the cart itself is unaffected by Reset() and stays inserted, so
    // this only needs the boot-sequence half repeated, not SetNDSCart().
    nds_->Reset();
    // Menu mode (see Load()'s boot_to_menu) has no cart and nothing to
    // direct-boot into -- the Reset() above already lands back on the
    // DSi Menu on its own, same as it did on the original boot.
    if (!loaded_rom_path_.empty()) {
        nds_->SetupDirectBoot(fs::path(loaded_rom_path_).filename().string());
    }
    nds_->Start();
}

bool MelonDSCore::InsertCart(const std::string& path) {
    // Only meaningful while sitting at a running system menu with no
    // cart (see Load()'s boot_to_menu) -- see this method's own doc
    // comment on EmulationCore for why a cart swap mid-game isn't
    // supported here.
    if (!nds_ || !loaded_ || !loaded_rom_path_.empty()) {
        return false;
    }
    if (!fs::exists(path)) {
        return false;
    }

    std::vector<uint8_t> romdata = ReadWholeFile(path);
    if (romdata.empty()) {
        return false;
    }
    auto rombuffer = std::make_unique<melonDS::u8[]>(romdata.size());
    std::memcpy(rombuffer.get(), romdata.data(), romdata.size());

    // Same homebrew-SDCard setup as Load() -- see its own comment on
    // why this is skipped for ordinary retail carts.
    melonDS::NDSCart::NDSCartArgs cart_args{};
    bool rom_is_homebrew = false;
    if (romdata.size() >= sizeof(melonDS::NDSHeader)) {
        melonDS::NDSHeader header{};
        std::memcpy(&header, romdata.data(), sizeof(header));
        rom_is_homebrew = header.IsHomebrew();
    }
    if (rom_is_homebrew) {
        const std::string homebrew_sdcard_root = BuildHomebrewSDCardRoot().string();
        const fs::path nds_sdcard_img = ToRealPath(
            fs::path(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir)) / "nds_sdcard.img");
        EnsureSDCardImageIsFresh(nds_sdcard_img, fs::path(homebrew_sdcard_root));
        cart_args.SDCard = melonDS::FATStorageArgs{
            nds_sdcard_img.string(),
            0,
            false,
            homebrew_sdcard_root,
        };
    }

    auto cart = melonDS::NDSCart::ParseROM(
        std::move(rombuffer), static_cast<melonDS::u32>(romdata.size()), nullptr,
        std::move(cart_args));
    if (!cart) {
        return false;
    }

    // Deliberately no Reset()/SetupDirectBoot()/Start() here -- the
    // console is already running (sitting at the DSi Menu, or whatever
    // else was already loaded via boot_to_menu), and the whole point is
    // for its own already-running code to notice and handle the newly-
    // inserted cart itself, exactly like a real console does when you
    // insert a cartridge while it's powered on.
    nds_->SetNDSCart(std::move(cart));
    loaded_rom_path_ = path;
    return true;
}

void MelonDSCore::FlushSave() {
    if (!nds_ || !loaded_)
        return;
    const uint8_t* save = nds_->GetNDSSave();
    uint32_t save_len = nds_->GetNDSSaveLength();
    if (save && save_len) {
        std::ofstream f(SaveDirFor(loaded_rom_path_), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(save), save_len);
    }
}

void MelonDSCore::Shutdown() {
    FlushSave();
    nds_.reset();
    loaded_ = false;
}

bool MelonDSCore::SaveState(const std::string& path) {
    if (!nds_)
        return false;

    melonDS::Savestate state(melonDS::Savestate::DEFAULT_SIZE);
    if (state.Error)
        return false;

    if (!nds_->DoSavestate(&state) || state.Error)
        return false;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;

    f.write(reinterpret_cast<const char*>(state.Buffer()), state.Length());
    return static_cast<bool>(f);
}

bool MelonDSCore::LoadState(const std::string& path) {
    if (!nds_ || !fs::exists(path))
        return false;

    std::vector<uint8_t> data = ReadWholeFile(path);
    if (data.empty())
        return false;

    melonDS::Savestate state(data.data(), static_cast<melonDS::u32>(data.size()), false);
    if (state.Error)
        return false;

    return nds_->DoSavestate(&state) && !state.Error;
}

} // namespace MergedCore
