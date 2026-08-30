// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <vector>
#include "core/hle/service/service.h"

struct AVCodecContext;
struct AVFrame;

namespace Service::MVD {

class MVD_STD final : public ServiceFramework<MVD_STD> {
public:
    MVD_STD();
    ~MVD_STD();

private:
    void Initialize(Kernel::HLERequestContext& ctx);
    void Shutdown(Kernel::HLERequestContext& ctx);
    void CalculateWorkBufSize(Kernel::HLERequestContext& ctx);
    void CalculateImageSize(Kernel::HLERequestContext& ctx);
    void SetupDecoder(Kernel::HLERequestContext& ctx);
    void FinalizeDecoder(Kernel::HLERequestContext& ctx);
    void InitializeInternal(Kernel::HLERequestContext& ctx);
    void ShutdownInternal(Kernel::HLERequestContext& ctx);
    void ConfigureDecoder(Kernel::HLERequestContext& ctx);
    void ProcessNALUnit(Kernel::HLERequestContext& ctx);
    void ControlFrameRendering(Kernel::HLERequestContext& ctx);
    void GetStatus(Kernel::HLERequestContext& ctx);
    void GetStatusOther(Kernel::HLERequestContext& ctx);
    void GetConfig(Kernel::HLERequestContext& ctx);
    void SetConfig(Kernel::HLERequestContext& ctx);
    void SetOutputBuffer(Kernel::HLERequestContext& ctx);
    void OverrideOutputBuffers(Kernel::HLERequestContext& ctx);

    bool EnsureDecoder();
    void ResetDecoder();
    bool RenderFrame();

    AVCodecContext* decoder{};
    AVFrame* decoded_frame{};
    std::array<u8, 0x11C> config{};
    bool have_config{};
    bool have_frame{};

    SERVICE_SERIALIZATION_SIMPLE
};

} // namespace Service::MVD

BOOST_CLASS_EXPORT_KEY(Service::MVD::MVD_STD)
