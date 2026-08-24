// src/core/three_ds_core_adapter.cpp

#include <fstream>

#include "core/three_ds_core_adapter.h"
#include "core/core.h"

namespace MergedCore {

ResultStatus ThreeDSCoreAdapter::MapStatus(int core_status) {
    // Core::System::ResultStatus values line up 1:1 in meaning with
    // MergedCore::ResultStatus for the cases both enums define; the
    // extra Azahar-specific ones (ErrorLoader_ErrorGbaTitle,
    // ErrorSavestateBuildMismatch, ErrorArticDisconnected, etc.) fold
    // down to ErrorUnknown here since the shared interface doesn't
    // need that level of detail — callers that need the exact reason
    // should read Core::System::GetStatusDetails() directly rather
    // than through this adapter.
    using CS = Core::System::ResultStatus;
    switch (static_cast<CS>(core_status)) {
    case CS::Success:
        return ResultStatus::Success;
    case CS::ErrorNotInitialized:
        return ResultStatus::ErrorNotInitialized;
    case CS::ErrorGetLoader:
        return ResultStatus::ErrorGetLoader;
    case CS::ErrorSystemMode:
        return ResultStatus::ErrorSystemMode;
    case CS::ErrorLoader:
        return ResultStatus::ErrorLoader;
    case CS::ErrorLoader_ErrorEncrypted:
        return ResultStatus::ErrorLoader_ErrorEncrypted;
    case CS::ErrorLoader_ErrorInvalidFormat:
        return ResultStatus::ErrorLoader_ErrorInvalidFormat;
    default:
        return ResultStatus::ErrorUnknown;
    }
}

ResultStatus ThreeDSCoreAdapter::Load(Frontend::EmuWindow& window, const std::string& path) {
    const auto status = Core::System::GetInstance().Load(window, path);
    return MapStatus(static_cast<int>(status));
}

void ThreeDSCoreAdapter::RunFrame(const InputState& /*input*/, FrameOutput& /*out*/) {
    // See the header comment: this intentionally does not touch
    // `out`. Existing Azahar rendering keeps working unmodified for
    // 3DS titles because it never went through this interface's
    // FrameOutput path to begin with — RunLoop() drives the same
    // renderer-to-window pipeline Azahar already has.
    [[maybe_unused]] const auto status = Core::System::GetInstance().RunLoop();
}

void ThreeDSCoreAdapter::Reset() {
    Core::System::GetInstance().Reset();
}

void ThreeDSCoreAdapter::Shutdown() {
    Core::System::GetInstance().Shutdown();
}

bool ThreeDSCoreAdapter::SaveState(const std::string& path) {
    // Core::System has no path-based save API — it's slot-based
    // (SaveState(u32 slot)) for its normal savestate UI, but it also
    // exposes SaveStateBuffer(), which hands back the serialized
    // state as raw bytes. Write those bytes to our own path so the
    // shared interface still gets a filesystem-path-based save
    // without duplicating Azahar's slot-management logic.
    const std::vector<u8> buffer = Core::System::GetInstance().SaveStateBuffer();
    if (buffer.empty())
        return false;

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
    return f.good();
}

bool ThreeDSCoreAdapter::LoadState(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;

    const auto size = f.tellg();
    f.seekg(0);
    std::vector<u8> buffer(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buffer.data()), size);
    if (!f.good())
        return false;

    return Core::System::GetInstance().LoadStateBuffer(std::move(buffer));
}

} // namespace MergedCore
