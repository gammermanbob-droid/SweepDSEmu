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
        first_frame_notified_ = false;
        rom_path_ = std::move(rom_path);
        run_thread_ = std::thread([this] { Run(); });
    }

    // auto_save_path empty = skip the auto-savestate (feature disabled, or
    // this is a load failure with nothing worth preserving).
    void Stop(const std::string& auto_save_path = {}) {
        if (!run_thread_.joinable()) {
            return;
        }
        if (!auto_save_path.empty()) {
            RequestSaveState(auto_save_path);
            // The Run() loop only consumes a pending save/load path once
            // per frame, in between RunFrame() calls -- if stop_requested_
            // were set immediately, the loop could observe it and exit
            // before ever reaching that check, silently dropping this
            // save. Wait for it to actually be consumed first. Bounded so
            // a wedged emulation thread can't hang shutdown forever; one
            // real frame at 60fps is ~16ms, so 2s is generous headroom.
            for (int i = 0; i < 200; ++i) {
                {
                    std::lock_guard lock(state_path_mutex_);
                    if (pending_save_path_.empty()) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        stop_requested_ = true;
        pause_requested_ = false;
        run_thread_.join();
    }

    void Pause() {
        pause_requested_.store(true, std::memory_order_relaxed);
        // Backgrounding is exactly when Android is most likely to kill
        // this process without warning -- flush now rather than only on
        // a clean Shutdown() that may never come. Consumed on the Run()
        // thread (see the main loop), since `core` is local to it.
        flush_save_requested_.store(true, std::memory_order_relaxed);
    }

    void Unpause() {
        pause_requested_.store(false, std::memory_order_relaxed);
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
        ApplyTopSurfaceGeometry();
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

    // Only called on the single-display (non-Thor/Odin) layout -- see
    // NativeLibrary.dsSetScreenGap's doc comment. The mere fact this was
    // ever called flips this session into combined-surface mode for good
    // (the Kotlin side decides this once per Activity, same as
    // usingSecondaryDisplay never changing mid-session).
    void SetScreenGap(int gap_px) {
        std::lock_guard lock(surface_mutex_);
        combined_mode_ = true;
        screen_gap_px_ = gap_px;
        ApplyTopSurfaceGeometry();
    }

private:
    static constexpr int kDsScreenWidth = 256;
    static constexpr int kDsScreenHeight = 192;

    // Caller must hold surface_mutex_.
    void ApplyTopSurfaceGeometry() {
        if (!top_surface_) {
            return;
        }
        if (combined_mode_) {
            ANativeWindow_setBuffersGeometry(top_surface_, kDsScreenWidth,
                                             2 * kDsScreenHeight + screen_gap_px_,
                                             WINDOW_FORMAT_RGBA_8888);
        } else {
            ANativeWindow_setBuffersGeometry(top_surface_, kDsScreenWidth, kDsScreenHeight,
                                             WINDOW_FORMAT_RGBA_8888);
        }
    }

    void Run();
    static void BlitFrame(ANativeWindow* surface, const MergedCore::ScreenBuffer& buffer);
    static void BlitCombinedFrame(ANativeWindow* surface, const MergedCore::ScreenBuffer& top,
                                   const MergedCore::ScreenBuffer& bottom, int gap_px);
    void InitAudio(int sample_rate);
    void WriteAudio(const std::vector<int16_t>& samples);
    void ShutdownAudio();
    void NotifyExit(int result_code);
    void NotifyFirstFrame();

    std::string rom_path_;
    std::thread run_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reset_requested_{false};
    std::atomic<bool> flush_save_requested_{false};

    std::atomic<bool> pause_requested_{false};
    std::atomic<bool> first_frame_notified_{false};

    std::mutex input_mutex_;
    MergedCore::InputState input_;

    std::mutex state_path_mutex_;
    std::string pending_save_path_;
    std::string pending_load_path_;

    std::mutex surface_mutex_;
    ANativeWindow* top_surface_ = nullptr;
    ANativeWindow* bottom_surface_ = nullptr;
    bool combined_mode_ = false;
    int screen_gap_px_ = 0;

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
    // WriteAudio feeds this in ~16ms-apart bursts straight from the DS
    // frame loop, not a steady small-callback cadence -- LOW_LATENCY's
    // tiny exclusive-MMAP burst buffer (~2ms on this hardware) can't
    // absorb normal scheduling jitter against that pattern, which is
    // what produced audible crackling and eventually a hard stream
    // disconnect. NONE uses the standard mixer path with a much deeper
    // buffer, trading a bit of extra latency (imperceptible for DS game
    // audio) for one that isn't perpetually on the edge of underrunning.
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // Without these, the stream defaults to AAUDIO_CONTENT_TYPE_UNKNOWN,
    // which some OEM audio policies (observed on a Samsung device here)
    // route/attenuate differently than a properly-tagged game/media
    // stream -- explicit tagging matches what every other audio output in
    // this app (3DS side, NDSBrewer's own menu music) already gets.
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    if (AAudioStreamBuilder_openStream(builder, &audio_stream_) != AAUDIO_OK) {
        LOG_ERROR(Frontend, "DS: failed to open AAudio stream");
        audio_stream_ = nullptr;
    }
    AAudioStreamBuilder_delete(builder);
    if (audio_stream_) {
        // Give the writer as much slack as the device will allow, on top
        // of the already-deeper NONE-mode buffer -- cheap insurance
        // against the same kind of underrun/crackle this is fixing.
        const int32_t capacity = AAudioStream_getBufferCapacityInFrames(audio_stream_);
        if (capacity > 0) {
            AAudioStream_setBufferSizeInFrames(audio_stream_, capacity);
        }
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

// Draws both DS screens into one SurfaceView's buffer (top screen, then a
// black gap, then the bottom screen) instead of using two separate
// SurfaceViews. Two co-existing SurfaceViews in the same window is a known
// Android compositor trigger for one of them getting stuck on its last
// frame after the Activity is covered and uncovered again (e.g. by
// Settings); a single surface has no second layer for the compositor to
// lose track of. Only used on the single-display (non-Thor/Odin) layout --
// see SetScreenGap.
void DsSession::BlitCombinedFrame(ANativeWindow* surface, const MergedCore::ScreenBuffer& top,
                                   const MergedCore::ScreenBuffer& bottom, int gap_px) {
    if (!surface || top.width != kDsScreenWidth || top.height != kDsScreenHeight ||
        top.pixels.size() != static_cast<size_t>(kDsScreenWidth) * kDsScreenHeight ||
        bottom.width != kDsScreenWidth || bottom.height != kDsScreenHeight ||
        bottom.pixels.size() != static_cast<size_t>(kDsScreenWidth) * kDsScreenHeight) {
        return;
    }
    ANativeWindow_Buffer window_buffer;
    if (ANativeWindow_lock(surface, &window_buffer, nullptr) != 0) {
        return;
    }

    auto* dst = reinterpret_cast<uint8_t*>(window_buffer.bits);
    const int stride_bytes = window_buffer.stride * 4;
    // melonDS's software renderer packs pixels as B,G,R,A in memory (see
    // upstream GPU_Soft.cpp), but the ANativeWindow buffer here is declared
    // WINDOW_FORMAT_RGBA_8888 (R,G,B,A), so swap channels while copying --
    // same as BlitFrame's single-surface case.
    auto blit_screen = [&](const MergedCore::ScreenBuffer& screen, int dst_y_offset) {
        const auto* src = reinterpret_cast<const uint8_t*>(screen.pixels.data());
        const int copy_height =
            std::min(kDsScreenHeight, window_buffer.height - dst_y_offset);
        for (int y = 0; y < copy_height; ++y) {
            const uint8_t* src_row = src + y * kDsScreenWidth * 4;
            uint8_t* dst_row = dst + (dst_y_offset + y) * stride_bytes;
            for (int x = 0; x < kDsScreenWidth; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        }
    };
    blit_screen(top, 0);
    const int bottom_y_offset = kDsScreenHeight + gap_px;
    for (int y = kDsScreenHeight; y < std::min(bottom_y_offset, window_buffer.height); ++y) {
        std::memset(dst + y * stride_bytes, 0, kDsScreenWidth * 4);
    }
    blit_screen(bottom, bottom_y_offset);

    ANativeWindow_unlockAndPost(surface);
}

void DsSession::NotifyExit(int result_code) {
    JNIEnv* env = IDCache::GetEnvForThread();
    env->CallStaticVoidMethod(IDCache::GetNativeLibraryClass(),
                              IDCache::GetExitDsEmulationActivity(), result_code);
}

void DsSession::NotifyFirstFrame() {
    JNIEnv* env = IDCache::GetEnvForThread();
    env->CallStaticVoidMethod(IDCache::GetNativeLibraryClass(), IDCache::GetNotifyDsFirstFrame());
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
    // Empty rom_path_ is MelonDSCore::Load's own "boot straight to the DSi
    // Menu, no cart" convention (see its boot_to_menu) -- must be left
    // alone here, since TranslateFilePath treats "" as a relative path
    // with nothing appended and resolves it to the SD root directory
    // itself, which then fails to load as neither a valid DS ROM nor a
    // 3DS one.
    if (!rom_path_.empty()) {
        rom_path_ = AndroidUtils::TranslateFilePath(rom_path_);
    }

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
    // Periodic safety-net autosave, on top of the pause-triggered one in
    // the loop below -- ~10s at the DS's ~59.8fps; cart save data is at
    // most a few hundred KB, so writing it out this often costs
    // essentially nothing.
    static constexpr int kAutosaveIntervalFrames = 600;
    int frames_since_save_flush = 0;

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (flush_save_requested_.exchange(false, std::memory_order_relaxed)) {
            core->FlushSave();
        }

        if (stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }

        // Deliberately NOT gating RunFrame() itself on pause_requested_
        // at all -- every attempt to slow down, skip, or discard
        // RunFrame() calls while "paused" (a blocking wait; a 100ms
        // poll loop that kept calling RunFrame() but discarded the
        // output) reproduced a real freeze after returning from
        // Settings: the picture on one screen would permanently stop
        // updating, confirmed via a per-frame pixel checksum that went
        // static at exactly that point, even though input kept arriving
        // and RunFrame() itself never stopped being called. The desktop
        // Qt frontend has no pause/resume concept for DS emulation at
        // all -- it just keeps calling RunFrame() completely normally,
        // always, matching what this now does -- and has never shown
        // this bug. WriteAudio() below is still skipped while paused so
        // audio doesn't keep playing in the background; BlitFrame()
        // already safely no-ops on its own once Android has torn down
        // the surfaces, so no separate skip is needed for it.
        const bool is_paused = pause_requested_.load(std::memory_order_relaxed);

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

        // Periodic safety-net flush while actively playing -- covers a
        // crash or an OS kill that happens to land in the foreground,
        // not just while backgrounded.
        if (++frames_since_save_flush >= kAutosaveIntervalFrames) {
            frames_since_save_flush = 0;
            core->FlushSave();
        }

        MergedCore::InputState input;
        {
            std::lock_guard lock(input_mutex_);
            input = input_;
        }

        MergedCore::FrameOutput frame;
        core->RunFrame(input, frame);

        // TEMPORARY diagnostic for the "bottom screen freezes/disappears
        {
            std::lock_guard lock(surface_mutex_);
            if (combined_mode_) {
                BlitCombinedFrame(top_surface_, frame.top, frame.bottom, screen_gap_px_);
            } else {
                BlitFrame(top_surface_, frame.top);
                BlitFrame(bottom_surface_, frame.bottom);
            }
        }
        // Tells the UI it can stop showing its "loading" screen -- one
        // real frame has actually been drawn now, as opposed to the
        // surfaces merely existing (which happens well before the ROM
        // has finished loading and started producing real output).
        if (!first_frame_notified_.exchange(true, std::memory_order_relaxed)) {
            NotifyFirstFrame();
        }
        if (!is_paused) {
            WriteAudio(frame.audio_samples);
        }

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

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsStopEmulation(
    JNIEnv* env, [[maybe_unused]] jobject obj, jstring j_auto_save_path) {
    GetSession().Stop(j_auto_save_path ? GetJString(env, j_auto_save_path) : std::string());
}

JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsPauseEmulation([[maybe_unused]] JNIEnv* env,
                                                         [[maybe_unused]] jobject obj) {
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsPauseEmulation");
    GetSession().Pause();
}

JNIEXPORT void JNICALL
Java_org_citra_citra_1emu_NativeLibrary_dsUnPauseEmulation([[maybe_unused]] JNIEnv* env,
                                                           [[maybe_unused]] jobject obj) {
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsUnPauseEmulation");
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
    // TEMPORARY diagnostic for the "bottom screen freezes after Settings"
    // investigation -- remove once the root cause is confirmed.
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsTopSurfaceChanged surf={}", surf != nullptr);
    GetSession().SetTopSurface(surf ? ANativeWindow_fromSurface(env, surf) : nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsSetScreenGap(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj, jint gap_px) {
    GetSession().SetScreenGap(gap_px);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsBottomSurfaceChanged(
    JNIEnv* env, [[maybe_unused]] jobject obj, jobject surf) {
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsBottomSurfaceChanged surf={}", surf != nullptr);
    GetSession().SetBottomSurface(surf ? ANativeWindow_fromSurface(env, surf) : nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsTopSurfaceDestroyed(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsTopSurfaceDestroyed");
    GetSession().SetTopSurface(nullptr);
}

JNIEXPORT void JNICALL Java_org_citra_citra_1emu_NativeLibrary_dsBottomSurfaceDestroyed(
    [[maybe_unused]] JNIEnv* env, [[maybe_unused]] jobject obj) {
    LOG_INFO(Frontend, "DS SURFACE DIAG: dsBottomSurfaceDestroyed");
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
