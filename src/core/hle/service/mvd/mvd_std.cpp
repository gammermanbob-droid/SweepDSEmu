// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include "common/dynamic_library/ffmpeg.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/mvd/mvd_std.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Service::MVD::MVD_STD)

namespace Service::MVD {

namespace {
constexpr u32 MVDStatusOK = 0x00017000;
constexpr u32 MVDStatusParameterSet = 0x00017001;
constexpr u32 MVDStatusFrameReady = 0x00017003;

u32 ReadConfigWord(const std::array<u8, 0x11C>& config, size_t offset) {
    u32 value;
    std::memcpy(&value, config.data() + offset, sizeof(value));
    return value;
}

u16 ToRGB565(int y, int u, int v, bool bgr) {
    const int c = std::max(0, y - 16);
    const int d = u - 128;
    const int e = v - 128;
    const int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
    const int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
    const int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);
    if (bgr)
        return static_cast<u16>(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
    return static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
} // namespace

static void LogMVDCommand(Kernel::HLERequestContext& ctx) {
    const auto header = ctx.CommandHeader();
    const u32 parameter_count =
        header.normal_params_size.Value() + header.translate_params_size.Value();
    std::string command = fmt::format("cmd=0x{:04X} header=0x{:08X}",
                                      header.command_id.Value(), header.raw);
    for (u32 i = 1; i <= parameter_count; ++i) {
        command += fmt::format(" p{}=0x{:08X}", i, ctx.CommandBuffer()[i]);
    }
    LOG_WARNING(Service, "MVD {}", command);
}

static void MVDReplySuccess(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);
    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        1, 0);
    rb.Push<u32>(0x00017000);
}

void MVD_STD::Initialize(Kernel::HLERequestContext& ctx) {
    ResetDecoder();
    MVDReplySuccess(ctx);
}

void MVD_STD::Shutdown(Kernel::HLERequestContext& ctx) {
    ResetDecoder();
    MVDReplySuccess(ctx);
}

void MVD_STD::CalculateWorkBufSize(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        2, 0);

    rb.Push(ResultSuccess);
    rb.Push<u32>(0x009006C8);
}

void MVD_STD::CalculateImageSize(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        2, 0);

    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void MVD_STD::SetupDecoder(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::FinalizeDecoder(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::InitializeInternal(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::ShutdownInternal(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::ConfigureDecoder(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::ProcessNALUnit(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestParser rp(ctx);
    const VAddr input_address = rp.Pop<u32>();
    const PAddr input_physical = rp.Pop<u32>();
    const u32 input_size = rp.Pop<u32>();
    [[maybe_unused]] const u32 sequence = rp.Pop<u32>();
    [[maybe_unused]] const u32 flags = rp.Pop<u32>();
    const auto process = rp.PopObject<Kernel::Process>();

    u32 status = MVDStatusParameterSet;
    if (process && input_size != 0 && EnsureDecoder()) {
        std::vector<u8> data(input_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        Core::System::GetInstance().Memory().ReadBlock(*process, input_address, data.data(),
                                                       input_size);
        AVPacket packet{};
        packet.data = data.data();
        packet.size = static_cast<int>(input_size);
        const int send_result = DynamicLibrary::FFmpeg::avcodec_send_packet(decoder, &packet);
        if (send_result >= 0) {
            DynamicLibrary::FFmpeg::av_frame_unref(decoded_frame);
            const int receive_result =
                DynamicLibrary::FFmpeg::avcodec_receive_frame(decoder, decoded_frame);
            if (receive_result >= 0) {
                have_frame = true;
                status = MVDStatusFrameReady;
                static bool logged_frame_layout = false;
                if (!logged_frame_layout) {
                    LOG_WARNING(Service,
                                "MVD decoded H.264 frame {}x{} format {} strides [{}, {}, {}]",
                                decoded_frame->width, decoded_frame->height,
                                decoded_frame->format, decoded_frame->linesize[0],
                                decoded_frame->linesize[1], decoded_frame->linesize[2]);
                    logged_frame_layout = true;
                }
            }
        } else {
            LOG_WARNING(Service, "MVD H.264 packet decode failed: {}", send_result);
        }
    }

    IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 4, 0);

    rb.Push<u32>(status);
    rb.Push<u32>(input_address + input_size);
    rb.Push<u32>(input_physical + input_size);
    rb.Push<u32>(0);
}

void MVD_STD::ControlFrameRendering(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);
    IPC::RequestParser rp(ctx);
    const u32 type = rp.Pop<u32>();
    [[maybe_unused]] const auto process = rp.PopObject<Kernel::Process>();
    if (type == 0 && have_frame && have_config)
        RenderFrame();
    IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 1, 0);
    rb.Push<u32>(MVDStatusOK);
}

void MVD_STD::GetStatus(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        17, 0);

    rb.Push<u32>(0x00017000);

    for (int i = 0; i < 16; ++i) {
        rb.Push<u32>(0);
    }
}

void MVD_STD::GetStatusOther(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        17, 0);

    rb.Push<u32>(0x00017000);

    for (int i = 0; i < 16; ++i) {
        rb.Push<u32>(0);
    }
}

void MVD_STD::GetConfig(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);

    IPC::RequestBuilder rb(
        ctx,
        static_cast<u16>(ctx.CommandHeader().command_id.Value()),
        2, 0);

    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void MVD_STD::SetConfig(Kernel::HLERequestContext& ctx) {
    LogMVDCommand(ctx);
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    [[maybe_unused]] const auto process = rp.PopObject<Kernel::Process>();
    auto& buffer = rp.PopMappedBuffer();
    if (size >= config.size()) {
        buffer.Read(config.data(), 0, config.size());
        have_config = true;
        LOG_WARNING(Service,
                  "MVD SetConfig input={}x{} output={}x{} format={:08X} paddr={:08X} "
                  "flag0x104={} val0={} val1={} w_override={} h_override={}",
                  ReadConfigWord(config, 0x0C), ReadConfigWord(config, 0x10),
                  ReadConfigWord(config, 0x5C), ReadConfigWord(config, 0x60),
                  ReadConfigWord(config, 0x58), ReadConfigWord(config, 0x64),
                  ReadConfigWord(config, 0x104), ReadConfigWord(config, 0x108),
                  ReadConfigWord(config, 0x10C), ReadConfigWord(config, 0x110),
                  ReadConfigWord(config, 0x114));
    }
    IPC::RequestBuilder rb(ctx, static_cast<u16>(ctx.CommandHeader().command_id.Value()), 1, 0);
    rb.Push<u32>(MVDStatusOK);
}

void MVD_STD::SetOutputBuffer(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

void MVD_STD::OverrideOutputBuffers(Kernel::HLERequestContext& ctx) {
    MVDReplySuccess(ctx);
}

MVD_STD::MVD_STD() : ServiceFramework("mvd:STD", 1) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &MVD_STD::Initialize, "Initialize"},
        {0x0002, &MVD_STD::Shutdown, "Shutdown"},
        {0x0003, &MVD_STD::CalculateWorkBufSize, "CalculateWorkBufSize"},
        {0x0004, &MVD_STD::CalculateImageSize, "CalculateImageSize"},
        {0x0005, &MVD_STD::SetupDecoder, "SetupDecoder"},
        {0x0007, &MVD_STD::FinalizeDecoder, "FinalizeDecoder"},
        {0x0008, &MVD_STD::ProcessNALUnit, "ProcessNALUnit"},
        {0x0009, &MVD_STD::ControlFrameRendering, "ControlFrameRendering"},
        {0x000A, &MVD_STD::GetStatus, "GetStatus"},
        {0x000B, &MVD_STD::GetStatusOther, "GetStatusOther"},
        {0x0018, &MVD_STD::InitializeInternal, "InitializeInternal"},
        {0x0019, &MVD_STD::ShutdownInternal, "ShutdownInternal"},
        {0x001B, &MVD_STD::ConfigureDecoder, "ConfigureDecoder"},
        {0x001C, &MVD_STD::FinalizeDecoder, "FinalizeDecoderInternal"},
        {0x001D, &MVD_STD::GetConfig, "GetConfig"},
        {0x001E, &MVD_STD::SetConfig, "SetConfig"},
        {0x001F, &MVD_STD::SetOutputBuffer, "SetOutputBuffer"},
        {0x0021, &MVD_STD::OverrideOutputBuffers, "OverrideOutputBuffers"} // clang-format on
    };

    RegisterHandlers(functions);
};

MVD_STD::~MVD_STD() {
    ResetDecoder();
}

bool MVD_STD::EnsureDecoder() {
    if (decoder)
        return true;
    if (!DynamicLibrary::FFmpeg::LoadH264Decoder()) {
        LOG_ERROR(Service, "MVD could not load FFmpeg");
        return false;
    }
    const AVCodec* codec = DynamicLibrary::FFmpeg::avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;
    decoder = DynamicLibrary::FFmpeg::avcodec_alloc_context3(codec);
    decoded_frame = DynamicLibrary::FFmpeg::av_frame_alloc();
    if (!decoder || !decoded_frame || DynamicLibrary::FFmpeg::avcodec_open2(decoder, codec, nullptr) < 0) {
        ResetDecoder();
        return false;
    }
    return true;
}

void MVD_STD::ResetDecoder() {
    if (decoded_frame)
        DynamicLibrary::FFmpeg::av_frame_free(&decoded_frame);
    if (decoder)
        DynamicLibrary::FFmpeg::avcodec_free_context(&decoder);
    have_frame = false;
    have_config = false;
}

bool MVD_STD::RenderFrame() {
    const u32 output_format = ReadConfigWord(config, 0x58);
    const u32 output_width = ReadConfigWord(config, 0x5C);
    const u32 output_height = ReadConfigWord(config, 0x60);
    const PAddr output_physical = ReadConfigWord(config, 0x64);
    if (!decoded_frame || !output_width || !output_height || !output_physical)
        return false;
    if (decoded_frame->format != AV_PIX_FMT_YUV420P &&
        decoded_frame->format != AV_PIX_FMT_YUVJ420P && decoded_frame->format != AV_PIX_FMT_NV12) {
        LOG_WARNING(Service, "MVD unsupported decoded pixel format {}", decoded_frame->format);
        return false;
    }

    const u32 override_flag = ReadConfigWord(config, 0x104);
    const u32 width_override = ReadConfigWord(config, 0x110);
    const u32 height_override = ReadConfigWord(config, 0x114);

    u32 effective_width = output_width;
    u32 effective_height = output_height;
    if (override_flag != 0) {
        if (width_override != 0) {
            effective_width = width_override;
        }
        if (height_override != 0) {
            effective_height = height_override;
        }
    }

    static bool logged_override = false;
    if (!logged_override) {
        LOG_WARNING(Service,
                    "MVD RenderFrame flag0x104={} w_override={} h_override={} "
                    "output={}x{} effective={}x{} decoded={}x{}",
                    override_flag, width_override, height_override, output_width, output_height,
                    effective_width, effective_height, decoded_frame->width, decoded_frame->height);
        logged_override = true;
    }

    const u32 buffer_width = effective_width;
    const u32 buffer_height = effective_height;
    const bool packed_565 = output_format == 0x00040002 || output_format == 0x00040004;
    if (!packed_565) {
        LOG_WARNING(Service, "MVD unsupported output format {:08X}", output_format);
        return false;
    }
    std::vector<u16> output(static_cast<size_t>(buffer_width) * buffer_height, 0);
    const u32 copy_width = std::min(buffer_width, static_cast<u32>(decoded_frame->width));
    const u32 copy_height = std::min(buffer_height, static_cast<u32>(decoded_frame->height));

    for (u32 y = 0; y < copy_height; ++y) {
        for (u32 x = 0; x < copy_width; ++x) {
            const int sx = static_cast<int>(x);
            const int sy = static_cast<int>(y);
            const int luma = decoded_frame->data[0][sy * decoded_frame->linesize[0] + sx];
            int u;
            int v;
            if (decoded_frame->format == AV_PIX_FMT_NV12) {
                const u8* uv = decoded_frame->data[1] + (sy / 2) * decoded_frame->linesize[1];
                u = uv[(sx / 2) * 2];
                v = uv[(sx / 2) * 2 + 1];
            } else {
                u = decoded_frame->data[1][(sy / 2) * decoded_frame->linesize[1] + sx / 2];
                v = decoded_frame->data[2][(sy / 2) * decoded_frame->linesize[2] + sx / 2];
            }
            output[static_cast<size_t>(y) * buffer_width + x] =
                ToRGB565(luma, u, v, false);
        }
    }
    const VAddr output_virtual = output_physical + 0x10000000;
    Core::System::GetInstance().Memory().WriteBlock(output_virtual, output.data(),
                                                    output.size() * sizeof(u16));
    LOG_DEBUG(Service, "MVD rendered frame to {:08X}", output_virtual);
    return true;
}

} // namespace Service::MVD
