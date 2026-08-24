// src/core/melonds_core/melon_ds_core.cpp

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "core/melonds_core/melon_ds_core.h"

#include "common/file_util.h"
#include "core/button_id_ds.h"

// melonDS core headers
#include "Args.h"
#include "DSi.h"
#include "DSi_NAND.h"
#include "FreeBIOS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "SPI_Firmware.h"

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
fs::path BuildHomebrewSDCardRoot() {
    const fs::path sdmc(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir));
    const fs::path root =
        fs::path(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) / "nds_sdcard_root";
    std::error_code ec;
    fs::create_directories(root, ec);

    for (const char* name : {"_nds", "roms"}) {
        const fs::path target = sdmc / name;
        if (!fs::is_directory(target, ec))
            continue;

        const fs::path link = root / name;
        if (fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == target)
            continue;

        fs::remove(link, ec);
        fs::create_directory_symlink(target, link, ec);
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
            const fs::path link = root / "moonshl2";
            if (!(fs::is_symlink(link, ec) && fs::read_symlink(link, ec) == moonshell_target)) {
                fs::remove(link, ec);
                fs::create_directory_symlink(moonshell_target, link, ec);
            }
        }
    }

    return root;
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

std::string MelonDSCore::SaveDirFor(const std::string& rom_path) const {
    // Azahar's existing save layout: <user_data>/sdmc/... for 3DS.
    // Route DS saves to a sibling directory instead of anywhere under
    // that tree so the two systems' save files can never shadow one
    // another, even if a 3DS and DS title happened to share a name.
    fs::path rom(rom_path);
    fs::path dir = fs::path(FileUtil::GetUserPath(FileUtil::UserPath::UserDir)) / "nds_saves";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / rom.stem()).string() + ".sav";
}

ResultStatus MelonDSCore::Load(Frontend::EmuWindow& /*window*/, const std::string& path) {
    if (!fs::exists(path)) {
        return ResultStatus::ErrorLoader;
    }

    // Discard any stale power-off signal left over from a previous
    // session — g_console_powered_off is a single global (see its
    // definition), not scoped to a particular NDS instance.
    melonDS::Platform::g_console_powered_off.store(false, std::memory_order_relaxed);

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
    // extra code here — just leave `args` alone.
    fs::path sysdata(FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir));
    fs::path bios9 = sysdata / "bios9.bin";
    fs::path bios7 = sysdata / "bios7.bin";
    fs::path firmware = sysdata / "firmware.bin";

    if (fs::exists(bios9) && fs::exists(bios7) && fs::exists(firmware)) {
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

    if (fs::exists(bios9) && fs::exists(bios7) && fs::exists(firmware)) {
        fs::path dsi_bios9_path = sysdata / "dsi_bios9.bin";
        fs::path dsi_bios7_path = sysdata / "dsi_bios7.bin";
        fs::path dsi_firmware_path = sysdata / "dsi_firmware.bin";
        fs::path dsi_nand_path = sysdata / "dsi_nand.bin";

        if (fs::exists(dsi_bios9_path) && fs::exists(dsi_bios7_path) &&
            fs::exists(dsi_firmware_path) && fs::exists(dsi_nand_path)) {
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

    if (dsi_nand.has_value()) {
        melonDS::DSiArgs dsi_args{
            std::move(args),
            std::move(dsi_bios9),
            std::move(dsi_bios7),
            std::move(dsi_nand),
            std::make_optional<melonDS::FATStorage>(melonDS::FATStorageArgs{
                FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "dsi_sdcard.img",
                0, // auto-size from the source directory's contents
                false,
                BuildHomebrewSDCardRoot().string(),
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
    melonDS::NDSCart::NDSCartArgs cart_args{};
    cart_args.SDCard = melonDS::FATStorageArgs{
        FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "nds_sdcard.img",
        0, // auto-size from the source directory's contents
        false,
        BuildHomebrewSDCardRoot().string(),
    };

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

    // Point save data at our own directory (see SaveDirFor) rather
    // than melonDS's default, which assumes it's the only emulator
    // touching the filesystem.
    std::vector<uint8_t> savedata = ReadWholeFile(SaveDirFor(path));
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
    if (nds_)
        nds_->Reset();
}

void MelonDSCore::Shutdown() {
    if (nds_ && loaded_) {
        const uint8_t* save = nds_->GetNDSSave();
        uint32_t save_len = nds_->GetNDSSaveLength();
        if (save && save_len) {
            std::ofstream f(SaveDirFor(loaded_rom_path_), std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(save), save_len);
        }
    }
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
