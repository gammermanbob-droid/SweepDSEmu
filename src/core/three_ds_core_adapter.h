// src/core/three_ds_core_adapter.h
//
// Written against the actual Core::System in this checkout's
// src/core/core.h (pasted 2026-08-23). Re-check this if you update
// Azahar to a newer commit later — Core::System's API isn't frozen
// either.

#pragma once

#include "core/emulation_core.h"

namespace MergedCore {

class ThreeDSCoreAdapter final : public EmulationCore {
public:
    SystemKind GetKind() const override { return SystemKind::ThreeDS; }

    ResultStatus Load(Frontend::EmuWindow& window, const std::string& path) override;

    // IMPORTANT: unlike MelonDSCore, this does NOT fill in `out`.
    // Core::System::RunLoop() doesn't take an input parameter or
    // return a framebuffer — the real 3DS renderer draws straight to
    // `window` on its own, and input reaches the emulated hardware
    // through Azahar's existing HID/window-callback path, not through
    // this call. `input` is accepted (to satisfy the interface) but
    // unused here; the 3DS side keeps behaving exactly as it does in
    // unmodified Azahar. See the "why FrameOutput doesn't fit both
    // cores" note in the project README before wiring this into the
    // renderer.
    void RunFrame(const InputState& input, FrameOutput& out) override;

    void Reset() override;
    void Shutdown() override;

    bool SaveState(const std::string& path) override;
    bool LoadState(const std::string& path) override;

    double GetTargetFPS() const override { return 59.8261; }

private:
    static ResultStatus MapStatus(int core_status); // Core::System::ResultStatus -> ours
};

} // namespace MergedCore
