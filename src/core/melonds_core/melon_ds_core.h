// src/core/melonds_core/melon_ds_core.h
//
// Wraps melonDS's headless core (its `NDS` class + friends, NOT its
// Qt/SDL frontend) so it satisfies MergedCore::EmulationCore.
//
// Build requirement: link against melonDS's `core` static library
// target (see cmake/melonds.cmake), which excludes
// src/frontend/qt_sdl/* — that directory is melonDS's own standalone
// UI and pulls in Qt/SDL dependencies you don't want duplicated
// inside Azahar, which already has its own Qt frontend.

#pragma once

#include <memory>
#include <string>

#include "core/emulation_core.h"

// melonDS headers (from the vendored/fetched melonDS source tree,
// see scripts/fetch_sources.sh)
#include "NDS.h"
#include "Args.h"
#include "GPU.h"
#include "SPU.h"

namespace MergedCore {

class MelonDSCore final : public EmulationCore {
public:
    MelonDSCore();
    ~MelonDSCore() override;

    SystemKind GetKind() const override { return SystemKind::DS; }

    ResultStatus Load(Frontend::EmuWindow& window, const std::string& path) override;
    void RunFrame(const InputState& input, FrameOutput& out) override;
    void Reset() override;
    void Shutdown() override;
    void FlushSave() override;

    bool SaveState(const std::string& path) override;
    bool LoadState(const std::string& path) override;

    double GetTargetFPS() const override { return 59.8261; }

    // Matches NDSArgs::OutputSampleRate's default in Load() below —
    // melonDS's SPU resamples to whatever rate it's constructed with,
    // so ReadAudio() always comes out at this rate regardless of the
    // DS's own internal ~32.7kHz mixing rate.
    int GetAudioSampleRate() const override { return 48000; }

private:
    // Drains whatever PCM melonDS's SPU has buffered since the last
    // call (bounded by nds_->RunFrame() above — melonDS mixes audio
    // alongside video, not on a separate cadence) into out.audio_samples.
    void ReadAudio(FrameOutput& out);

    // Maps MergedCore::InputState's generic bitmask onto the bit
    // positions NDS::SetKeyMask expects (A/B/Select/Start/Right/Left/
    // Up/Down/R/L/X/Y — note the DS has no shoulder-Z, no circle pad,
    // no C-stick, so several 3DS buttons have no DS equivalent and are
    // dropped here rather than mismapped).
    void ApplyInput(const InputState& input);

    // melonDS's GPU exposes the two screens as separate RAM
    // framebuffers (GPU::GetFramebuffers); this copies them into
    // Azahar's ScreenBuffer pair and applies melonDS's own
    // top/bottom swap setting.
    void SplitFramebuffer(FrameOutput& out);

    // Separate save/state directory so DS titles never collide with
    // 3DS save data or savestate slots that share a title ID
    // namespace by coincidence.
    std::string SaveDirFor(const std::string& rom_path) const;

    std::unique_ptr<melonDS::NDS> nds_;
    bool loaded_ = false;
    std::string loaded_rom_path_;

    static constexpr int kScreenWidth = 256;
    static constexpr int kScreenHeight = 192;
};

} // namespace MergedCore
