// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// JNI bridge for DS/DSi emulation (MergedCore::EmulationCore /
// MelonDsCore), parallel to native.cpp's 3DS-only path. Unlike the 3DS
// renderer, DS EmulationCore::RunFrame() hands back plain RGBA8888 pixel
// buffers per frame (see core/emulation_core.h) rather than driving a
// GL/Vulkan context itself, so this bridges via
// ANativeWindow_lock/unlockAndPost software blits and the NDK AAudio API
// instead of reusing emu_window_gl.cpp/emu_window_vk.cpp. See
// ds_player_window.cpp for the same shape of bridge on the Qt frontend.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <aaudio/AAudio.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "common/android_utils.h"
#include "common/logging/log.h"
#include "core/core_factory.h"
#include "core/emulation_core.h"
#include "core/frontend/emu_window.h"
#include "jni/android_common/android_common.h"
#include "jni/id_cache.h"

namespace {

// MergedCore::EmulationCore::Load() takes a Frontend::EmuWindow& purely to
// satisfy the shared interface (Ctr3DSCoreAdapter needs a real one to hand
// to Core::System::Load); MelonDsCore itself never reads it. Same stub as
// ds_player_window.cpp's NullEmuWindow.
class NullEmuWindow : public Frontend::EmuWindow {
public:
    void PollEvents() override {}
};

class DsSession {
public:
    void Start(std::string rom_path) {
        Stop();
        stop_requested_ = false;
        pause_requested_ = false;
        rom_path_ = std::move(rom_path);
        run_thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        if (!run_thread_.joinable()) {
            return;
        }
        stop_requested_ = true;
        {
            std::lock_guard lock(pause_mutex_);
            pause_requested_ = false;
        }
        pause_cv_.notify_all();
        run_thread_.join();
    }

    void Pause() {
        std::lock_guard lock(pause_mutex_);
        pause_requested_ = true;
    }

    void Unpause() {
        {
            std::lock_guard lock(pause_mutex_);
            pause_requested_ = false;
        }
        pause_cv_.notify_all();
    }

    bool IsRunning() const {
        return run_thread_.joinable() && !stop_requested_.load(std::memory_order_relaxed);
    }

    void SetButton(uint32_t button_bit, bool pressed) {
        std::lock_guard lock(input_mutex_);
        if (pressed) {
            input_.buttons |= button_bit;
        } else {
            input_.buttons &= ~button_bit;
        }
    }

    void SetTouch(bool pressed, uint16_t x, uint16_t y) {
        std::lock_guard lock(input_mutex_);
        input_.touch_pressed = pressed;
        if (pressed) {
            input_.touch_x = x;
            input_.touch_y = y;
        }
    }

    void RequestReset() {
        reset_requested_ = true;
    }

    void RequestSaveState(std::string path) {
        std::lock_guard lock(state_path_mutex_);
        pending_save_path_ = std::move(path);
    }

    void RequestLoadState(std::string path) {
        std::lock_guard lock(state_path_mutex_);
        pending_load_path_ = std::move(path);
    }

    void SetTopSurface(ANativeWindow* surface) {
        std::lock_guard lock(surface_mutex_);
        if (top_surface_) {
            ANativeWindow_release(top_surface_);
        }
        top_surface_ = surface;
        if (top_surface_) {
            ANativeWindow_setBuffersGeometry(top_surface_, kDsScreenWidth, kDsScreenHeight,
                                             WINDOW_FORMAT_RGBA_8888);
        }
    }

    void SetBottomSurface(ANativeWindow* surface) {
        std::lock_guard lock(surface_mutex_);
        if (bottom_surface_) {
            ANativeWindow_release(bottom_surface_);
        }
        bottom_surface_ = surface;
        if (bottom_surface_) {
            ANativeWindow_setBuffersGeometry(bottom_surface_, kDsScreenWidth, kDsScreenHeight,
                                             WINDOW_FORMAT_RGBA_8888);
        }
    }

private:
    static constexpr int kDsScreenWidth = 256;
    static constexpr int kDsScreenHeight = 192;

    void Run();
    static void BlitFrame(ANativeWindow* surface, const MergedCore::ScreenBuffer& buffer);
    void InitAudio(int sample_rate);
    void WriteAudio(const std::vector<int16_t>& samples);
    void ShutdownAudio();
    void NotifyExit(int result_code);

    std::string rom_path_;
    std::thread run_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reset_requested_{false};

    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    bool pause_requested_ = false;

    std::mutex input_mutex_;
    MergedCore::InputState input_;

    std::mutex state_path_mutex_;
    std::string pending_save_path_;
    std::string pending_load_path_;

    std::mutex surface_mutex_;
    ANativeWindow* top_surface_ = nullptr;
    ANativeWindow* bottom_surface_ = nullptr;

    AAudioStream* audio_stream_ = nullptr;
    int audio_sample_rate_ = 0;
};

void DsSession::InitAudio(int sample_rate) {
    if (sample_rate <= 0) {
        return;
    }
    audio_sample_rate_ = sample_rate;
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        LOG_ERROR(Frontend, "DS: failed to create AAudio stream builder");
        return;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    if (AAudioStreamBuilder_openStream(builder, &audio_stream_) != AAUDIO_OK) {
        LOG_ERROR(Frontend, "DS: failed to open AAudio stream");
        audio_stream_ = nullptr;
    }
    AAudioStreamBuilder_delete(builder);
    if (audio_stream_) {
        AAudioStream_requestStart(audio_stream_);
    }
}

void DsSession::WriteAudio(const std::vector<int16_t>& samples) {
    if (!audio_stream_ || samples.empty()) {
        return;
    }
    const int32_t frames = static_cast<int32_t>(samples.size() / 2);
    // Non-blocking (timeout 0): DS frame pacing is already correct in
    // Run()'s loop, and an occasional audio underrun is far less
    // disruptive than stalling the emulation thread waiting for a full
    // AAudio ring buffer to drain.
    const aaudio_result_t result =
        AAudioStream_write(audio_stream_, samples.data(), frames, 0);
    if (result < 0) {
        // A negative result here (most commonly AAUDIO_ERROR_DISCONNECTED,
        // e.g. a Bluetooth/wired route change or another app grabbing the
        // low-latency MMAP path) means this stream is now permanently
        // dead -- AAudio streams can't be resumed after disconnecting,
        // only replaced. Without this, a single disconnect silently and
        // irrecoverably killed DS audio for the rest of the session even
        // though nothing else was wrong.
        LOG_ERROR(Frontend, "DS: AAudio write failed ({}), reopening stream",
                  AAudio_convertResultToText(result));
        AAudioStream_close(audio_stream_);
        audio_stream_ = nullptr;
        InitAudio(audio_sample_rate_);
    }
}

void DsSession::ShutdownAudio() {
    if (audio_stream_) {
        AAudioStream_requestStop(audio_stream_);
        AAudioStream_close(audio_stream_);
        audio_stream_ = nullptr;
    }
}

void DsSession::BlitFrame(ANativeWindow* surface, const MergedCore::ScreenBuffer& buffer) {
    if (!surface || buffer.width <= 0 || buffer.height <= 0 ||
        buffer.pixels.size() != static_cast<size_t>(buffer.width) * buffer.height) {
        return;
    }
    ANativeWindow_Buffer window_buffer;
    if (ANativeWindow_lock(surface, &window_buffer, nullptr) != 0) {
        return;
    }
    const int copy_height = std::min(buffer.height, window_buffer.height);
    const int copy_width = std::min(buffer.width, window_buffer.width);
    auto* dst = reinterpret_cast<uint8_t*>(window_buffer.bits);
    const auto* src = reinterpret_cast<const uint8_t*>(buffer.pixels.data());
    // melonDS's software renderer packs pixels as B,G,R,A in memory (see
    // upstream GPU_Soft.cpp), but the ANativeWindow buffer here is declared
    // WINDOW_FORMAT_RGBA_8888 (R,G,B,A). A raw memcpy would silently swap
    // the red and blue channels, so swap them back per pixel while copying.
    for (int y = 0; y < copy_height; ++y) {
        const uint8_t* src_row = src + y * buffer.width * 4;
        uint8_t* dst_row = dst + y * window_buffer.stride * 4;
        for (int x = 0; x < copy_width; ++x) {
            dst_row[x * 4 + 0] = src_row[x * 4 + 2];
            dst_row[x * 4 + 1] = src_row[x * 4 + 1];
            dst_row[x * 4 + 2] = src_row[x * 4 + 0];
            dst_row[x * 4 + 3] = src_row[x * 4 + 3];
        }
    }
    ANativeWindow_unlockAndPost(surface);
}

void DsSession::NotifyExit(int result_code) {
    JNIEnv* env = IDCache::GetEnvForThread();
    env->CallStaticVoidMethod(IDCache::GetNativeLibraryClass(),
                              IDCache::GetExitDsEmulationActivity(), result_code);
}

void DsSession::Run() {
    // Paths coming from Android's file picker are either citra's own
    // "!<real path>" convention (used to route around a missing native
    // path for a SAF-picked file, see GameHelper.kt/Game.kt) or relative
    // to the user directory -- FileUtil::IOFile and friends handle this
    // transparently for every 3DS file access (see
    // AndroidUtils::TranslateFilePath's callers in common/file_util.cpp),
    // but melonDS's own file I/O has no idea about any of that, so
    // without this translation every DS ROM load from the game list
    // fails silently (CoreFactory::CreateFor can't even sniff the header
    // of a literal "!/storage/..." path) and the player just bounces
    // straight back out.
    rom_path_ = AndroidUtils::TranslateFilePath(rom_path_);

    NullEmuWindow window;
    auto core = MergedCore::CoreFactory::CreateFor(rom_path_);
    if (!core) {
        LOG_ERROR(Frontend, "DS: file not recognized as a DS/DSi ROM: {}", rom_path_);
        NotifyExit(-1);
        return;
    }

    const auto load_status = core->Load(window, rom_path_);
    if (load_status != MergedCore::ResultStatus::Success) {
        LOG_ERROR(Frontend, "DS: failed to load ROM (status {})", static_cast<int>(load_status));
        NotifyExit(static_cast<int>(load_status));
        return;
    }

    InitAudio(core->GetAudioSampleRate());

    const auto frame_period = std::chrono::duration<double>(1.0 / core->GetTargetFPS());
    auto next_frame = std::chrono::steady_clock::now();
    bool console_powered_off = false;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock lock(pause_mutex_);
            pause_cv_.wait(lock, [this] {
                return !pause_requested_ || stop_requested_.load(std::memory_order_relaxed);
            });
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }

        {
            std::lock_guard lock(state_path_mutex_);
            if (!pending_save_path_.empty()) {
                core->SaveState(pending_save_path_);
                pending_save_path_.clear();
            }
            if (!pending_load_path_.empty()) {
                core->LoadState(pending_load_path_);
                pending_load_path_.clear();
            }
        }

        if (reset_requested_.exchange(false)) {
            core->Reset();
        }

        MergedCore::InputState input;
        {
            std::lock_guard lock(input_mutex_);
            input = input_;
        }

        MergedCore::FrameOutput frame;
        core->RunFrame(input, frame);

        {
            std::lock_guard lock(surface_mutex_);
            BlitFrame(top_surface_, frame.top);
            BlitFrame(bottom_surface_, frame.bottom);
        }
        WriteAudio(frame.audio_samples);

        if (frame.powered_off) {
            // core->RunFrame() is a permanent no-op from here on (mirrors
            // MelonDSCore's behavior on the Qt frontend) -- stop asking
            // for more frames instead of spinning on a frozen picture.
            console_powered_off = true;
            break;
        }

        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_period);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            // Fell behind -- don't try to burst-catch-up, resync from here.
            next_frame = now;
        }
    }

    core->Shutdown();
    ShutdownAudio();

    {
        std::lock_guard lock(surface_mutex_);
        if (top_surface_) {
            ANativeWindow_release(top_surface_);
            top_surface_ = nullptr;
        }
        if (bottom_surface_) {
            ANativeWindow_release(bottom_surface_);
            bottom_surface_ = nullptr;
        }
    }

    // 0 = clean stop requested from Kotlin (Stop()); 1 = the console
    // powered itself off (real hardware behavior, not an error) -- both
    // are handled identically by exitDsEmulationActivity (just finish the
    // Activity), matching CoreError.ShutdownRequested's meaning for the
    // 3DS path. A negative value indicates an actual load failure.
    NotifyExit(console_powered_off ? 1 : 0);
}

DsSession& GetSession() {
    static DsSession session;
    return session;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsRun(JNIEnv* env,
                                                                     [[maybe_unused]] jobject obj,
                                                                     jstring j_path) {
    GetSession().Start(GetJString(env, j_path));
}

JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsStopEmulation([[maybe_unused]] JNIEnv* env,
                                                        [[maybe_unused]] jobject obj) {
    GetSession().Stop();
}

JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsPauseEmulation([[maybe_unused]] JNIEnv* env,
                                                         [[maybe_unused]] jobject obj) {
    GetSession().Pause();
}

JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsUnPauseEmulation([[maybe_unused]] JNIEnv* env,
                                                           [[maybe_unused]] jobject obj) {
    GetSession().Unpause();
}

JNIEXPORT jboolean JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsIsRunning([[maybe_unused]] JNIEnv* env,
                                                    [[maybe_unused]] jobject obj) {
    return GetSession().IsRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsRequestReset(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    GetSession().RequestReset();
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsSaveState(JNIEnv* env,
                                                                           [[maybe_unused]] jobject obj,
                                                                           jstring j_path) {
    GetSession().RequestSaveState(GetJString(env, j_path));
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsLoadState(JNIEnv* env,
                                                                           [[maybe_unused]] jobject obj,
                                                                           jstring j_path) {
    GetSession().RequestLoadState(GetJString(env, j_path));
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsTopSurfaceChanged(
    JNIEnv* env, [[maybe_unused]] jobject obj, jobject surf) {
    GetSession().SetTopSurface(surf ? ANativeWindow_fromSurface(env, surf) : nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsBottomSurfaceChanged(
    JNIEnv* env, [[maybe_unused]] jobject obj, jobject surf) {
    GetSession().SetBottomSurface(surf ? ANativeWindow_fromSurface(env, surf) : nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsTopSurfaceDestroyed(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    GetSession().SetTopSurface(nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsBottomSurfaceDestroyed(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    GetSession().SetBottomSurface(nullptr);
}

// button_bit is one of MergedCore::DSButton (core/button_id_ds.h), sent
// pre-resolved from Kotlin's DsButtonType rather than a per-button ID +
// factory lookup like the 3DS's onGamePadEvent -- there's no separate
// Input::ButtonDevice registry for DS, InputState is just a plain bitmask
// the emulation thread reads once per frame.
JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsOnButtonEvent(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jint button_bit,
    jboolean pressed) {
    GetSession().SetButton(static_cast<uint32_t>(button_bit), pressed == JNI_TRUE);
}

// x/y are already in 256x192 DS bottom-screen space -- Kotlin scales from
// the SurfaceView's pixel coordinates before calling this, mirroring
// DSPlayerWindow::UpdateTouch's scale_x/scale_y on the Qt frontend.
JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsOnTouchEvent(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jint x, jint y,
    jboolean pressed) {
    GetSession().SetTouch(pressed == JNI_TRUE, static_cast<uint16_t>(x), static_cast<uint16_t>(y));
}

} // extern "C"
