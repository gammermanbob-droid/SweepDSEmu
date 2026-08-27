// include/core/emulation_core.h
//
// Common abstraction that lets Azahar's frontend (Qt UI, input system,
// screen layout engine, savestate manager, etc.) drive either the
// existing 3DS core or a new DS core without caring which one is loaded.
//
// Drop this into: azahar/src/core/emulation_core.h
// It intentionally mirrors the shape of Azahar's existing `Core::System`
// entry points (Load / RunLoop / GetStatus) so wiring it in is a matter
// of making Core::System implement this interface too, rather than
// rewriting the frontend.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Frontend {
class EmuWindow;
}

namespace MergedCore {

enum class SystemKind {
    ThreeDS,
    DS,
};

enum class ResultStatus {
    Success,
    ErrorNotInitialized,
    ErrorGetLoader,
    ErrorSystemMode,
    ErrorLoader,
    ErrorLoader_ErrorEncrypted,
    ErrorLoader_ErrorInvalidFormat,
    ErrorVideoCore,
    ErrorUnknown,
};

// Framebuffer handed back to the frontend for presentation. Both cores
// fill this in per-screen so the existing dual-screen layout code
// (top/bottom, side-by-side, swap, gap, fold — see
// azahar/src/video_core/renderer_*/layout.cpp) can treat a DS's two
// 256x192 screens the same way it already treats the 3DS's 400x240 top
// / 320x240 bottom screens.
struct ScreenBuffer {
    std::vector<uint32_t> pixels; // RGBA8888, row-major
    int width = 0;
    int height = 0;
};

struct FrameOutput {
    ScreenBuffer top;    // 3DS "top" screen OR DS top screen
    ScreenBuffer bottom; // 3DS "bottom" screen OR DS bottom screen

    // Interleaved stereo S16 PCM produced during this frame, at
    // GetAudioSampleRate() — empty if this core doesn't (yet) surface
    // audio through this path. Batched per-frame rather than pushed
    // via a separate callback since both cores already produce exactly
    // one FrameOutput per RunFrame() call.
    std::vector<int16_t> audio_samples;

    // True exactly once, on the frame during which the emulated system
    // shut itself down (e.g. DS/DSi firmware settings finishing their
    // "will now power off" sequence — real hardware behavior, not an
    // error). The frontend should stop driving RunFrame() and end the
    // session cleanly rather than keep calling into an instance that
    // will never produce another frame.
    bool powered_off = false;
};

// Minimal input snapshot. The real Azahar input system
// (src/input_common/*) already normalizes physical devices into a
// button/circlepad/touch struct — this is the subset a DS core needs.
struct InputState {
    uint32_t buttons = 0; // bitmask, see button_id_ds.h / button_id_3ds.h
    bool touch_pressed = false;
    uint16_t touch_x = 0; // bottom-screen space, core-specific resolution
    uint16_t touch_y = 0;
};

class EmulationCore {
public:
    virtual ~EmulationCore() = default;

    virtual SystemKind GetKind() const = 0;

    // Load a ROM. `path` has already been extension/header-sniffed by
    // CoreFactory (see core_factory.h) before a core is even
    // constructed, so implementations can assume the file matches
    // their system.
    virtual ResultStatus Load(Frontend::EmuWindow& window, const std::string& path) = 0;

    // Run exactly one emulated video frame and hand back both screens.
    virtual void RunFrame(const InputState& input, FrameOutput& out) = 0;

    virtual void Reset() = 0;
    virtual void Shutdown() = 0;

    // Persists native cart save data (SRAM/EEPROM/Flash) to disk right
    // now, without shutting down -- otherwise that data only ever hits
    // disk on a clean Shutdown(), which never happens if the process is
    // killed while backgrounded (Android can do this at any time) or
    // crashes, silently losing whatever the game itself had "saved"
    // since the last clean exit. No-op by default: cores whose save
    // data is already written directly by their own filesystem layer
    // (e.g. the 3DS side, via Service::FS) have nothing to flush here.
    virtual void FlushSave() {}

    // Savestates. Both cores serialize to an opaque byte blob; the
    // frontend is responsible for choosing a per-core save directory
    // (see melon_ds_core.cpp's SaveDirFor()) so 3DS and DS states never
    // collide.
    virtual bool SaveState(const std::string& path) = 0;
    virtual bool LoadState(const std::string& path) = 0;

    virtual double GetTargetFPS() const = 0; // 59.83 (3DS) or 59.82 (DS)

    // Sample rate of FrameOutput::audio_samples, or 0 if this core
    // doesn't surface audio through that path (e.g. the 3DS adapter,
    // which still uses Azahar's existing audio_core pipeline directly).
    virtual int GetAudioSampleRate() const { return 0; }
};

} // namespace MergedCore
