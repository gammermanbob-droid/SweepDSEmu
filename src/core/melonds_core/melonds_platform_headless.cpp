// src/core/melonds_core/melonds_platform_headless.cpp
//
// melonDS's Platform.h is a "the frontend implements this" contract —
// melonDS itself only ships one implementation, in
// src/frontend/qt_sdl/Platform.cpp, which pulls in QFile/QThread/
// QSemaphore and friends. We deliberately don't build melonDS's own
// Qt/SDL frontend (see cmake/melonds.cmake), so nothing else defines
// these symbols; the core links fine as its own static library but
// fails at final link time once anything actually calls into it,
// since every Platform:: function below is otherwise just a dangling
// declaration.
//
// Multiplayer (MP_*), networking (Net_*), camera, microphone, and AAC
// decoding are all no-ops here — none of that applies to a ROM
// launched straight from Azahar's frontend. File I/O, logging, and
// the threading primitives are real, implemented directly against the
// C++ standard library rather than Qt.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <semaphore>
#include <thread>

#include "Platform.h"
#include "SPI_Firmware.h"

namespace melonDS::Platform {

namespace fs = std::filesystem;

// NDS::Stop() sets Running = false unconditionally (see NDS.cpp) — once
// that happens, NDS::RunFrame()'s main loop is permanently a no-op for
// the rest of this NDS instance's lifetime, silently freezing output
// forever unless something notices and stops asking for more frames.
// PowerOff specifically is *expected*, not an error: real DS/DSi
// firmware genuinely shuts the console all the way down after its
// first-boot setup wizard saves settings (unlike a soft-reset, which
// never reaches NDS::Stop at all). One instance runs at a time in this
// embedding, so a plain global is simpler than plumbing UserData
// through every Load() callsite — see MelonDSCore::RunFrame(), which
// polls and clears this once per frame.
std::atomic<bool> g_console_powered_off{false};

void SignalStop(StopReason reason, void* /*userdata*/) {
    if (reason == StopReason::PowerOff) {
        g_console_powered_off.store(true, std::memory_order_relaxed);
    }
}

// FileHandle is intentionally left an opaque forward declaration by
// melonDS (see Platform.h's comment on it) specifically so a frontend
// can point it at whatever it likes via reinterpret_cast — same
// pattern melonDS's own Qt frontend uses, just aliasing a plain FILE*
// instead of a QFile*.

namespace {

constexpr char AccessMode(FileMode mode, bool file_exists) {
    if (mode & FileMode::Append)
        return 'a';
    if (!(mode & FileMode::Write))
        return 'r';
    if (mode & FileMode::NoCreate)
        return 'r';
    if ((mode & FileMode::Preserve) && file_exists)
        return 'r';
    return 'w';
}

constexpr bool IsExtended(FileMode mode) {
    return (mode & FileMode::ReadWrite) == FileMode::ReadWrite;
}

std::string GetModeString(FileMode mode, bool file_exists) {
    std::string s;
    s += AccessMode(mode, file_exists);
    if (IsExtended(mode))
        s += '+';
    if (!(mode & FileMode::Text))
        s += 'b';
    return s;
}

} // namespace

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None) {
        Log(LogLevel::Error, "Attempted to open \"%s\" in neither read nor write mode\n",
            path.c_str());
        return nullptr;
    }

    const bool exists = fs::exists(path);
    const std::string mode_str = GetModeString(mode, exists);
    FILE* f = std::fopen(path.c_str(), mode_str.c_str());
    return reinterpret_cast<FileHandle*>(f);
}

std::string GetLocalFilePath(const std::string& filename) {
    // No concept of a distinct "local" (relative-to-install) path
    // separate from the caller-provided path here — MelonDSCore
    // already resolves every path it hands to OpenFile against
    // Azahar's own user data directories before calling in.
    return filename;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) {
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name) {
    return fs::exists(name);
}

bool LocalFileExists(const std::string& name) {
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath) {
    FILE* f = std::fopen(filepath.c_str(), "ab");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath) {
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file) {
    if (!file)
        return false;
    return std::fclose(reinterpret_cast<FILE*>(file)) == 0;
}

bool IsEndOfFile(FileHandle* file) {
    return std::feof(reinterpret_cast<FILE*>(file)) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file) {
    return std::fgets(str, count, reinterpret_cast<FILE*>(file)) != nullptr;
}

u64 FilePosition(FileHandle* file) {
    return static_cast<u64>(std::ftell(reinterpret_cast<FILE*>(file)));
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
    int whence = SEEK_SET;
    switch (origin) {
    case FileSeekOrigin::Start:
        whence = SEEK_SET;
        break;
    case FileSeekOrigin::Current:
        whence = SEEK_CUR;
        break;
    case FileSeekOrigin::End:
        whence = SEEK_END;
        break;
    }
    return std::fseek(reinterpret_cast<FILE*>(file), static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file) {
    std::rewind(reinterpret_cast<FILE*>(file));
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) {
    return std::fread(data, size, count, reinterpret_cast<FILE*>(file));
}

bool FileFlush(FileHandle* file) {
    return std::fflush(reinterpret_cast<FILE*>(file)) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) {
    return std::fwrite(data, size, count, reinterpret_cast<FILE*>(file));
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int written = std::vfprintf(reinterpret_cast<FILE*>(file), fmt, args);
    va_end(args);
    return written < 0 ? 0 : static_cast<u64>(written);
}

u64 FileLength(FileHandle* file) {
    FILE* f = reinterpret_cast<FILE*>(file);
    const long pos = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    return len < 0 ? 0 : static_cast<u64>(len);
}

void Log(LogLevel level, const char* fmt, ...) {
    const char* prefix = "melonDS";
    switch (level) {
    case LogLevel::Debug:
        prefix = "melonDS/Debug";
        break;
    case LogLevel::Info:
        prefix = "melonDS/Info";
        break;
    case LogLevel::Warn:
        prefix = "melonDS/Warn";
        break;
    case LogLevel::Error:
        prefix = "melonDS/Error";
        break;
    }
    std::fprintf(stderr, "[%s] ", prefix);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

// --- Threading primitives — thin wrappers over the standard library,
// reinterpret_cast through melonDS's opaque Thread/Semaphore/Mutex
// tags the same way FileHandle is handled above. ---

Thread* Thread_Create(std::function<void()> func) {
    return reinterpret_cast<Thread*>(new std::thread(std::move(func)));
}

void Thread_Free(Thread* thread) {
    auto* t = reinterpret_cast<std::thread*>(thread);
    if (t->joinable())
        t->detach();
    delete t;
}

void Thread_Wait(Thread* thread) {
    auto* t = reinterpret_cast<std::thread*>(thread);
    if (t->joinable())
        t->join();
}

Semaphore* Semaphore_Create() {
    return reinterpret_cast<Semaphore*>(new std::counting_semaphore<>(0));
}

void Semaphore_Free(Semaphore* sema) {
    delete reinterpret_cast<std::counting_semaphore<>*>(sema);
}

void Semaphore_Reset(Semaphore* sema) {
    // std::counting_semaphore has no reset-to-zero API; drain it
    // instead by acquiring until empty. try_acquire() never blocks,
    // so this terminates even if the count is already 0.
    auto* s = reinterpret_cast<std::counting_semaphore<>*>(sema);
    while (s->try_acquire()) {
    }
}

void Semaphore_Wait(Semaphore* sema) {
    reinterpret_cast<std::counting_semaphore<>*>(sema)->acquire();
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
    auto* s = reinterpret_cast<std::counting_semaphore<>*>(sema);
    if (timeout_ms <= 0)
        return s->try_acquire();
    return s->try_acquire_for(std::chrono::milliseconds(timeout_ms));
}

void Semaphore_Post(Semaphore* sema, int count) {
    reinterpret_cast<std::counting_semaphore<>*>(sema)->release(count);
}

Mutex* Mutex_Create() {
    return reinterpret_cast<Mutex*>(new std::recursive_mutex());
}

void Mutex_Free(Mutex* mutex) {
    delete reinterpret_cast<std::recursive_mutex*>(mutex);
}

void Mutex_Lock(Mutex* mutex) {
    reinterpret_cast<std::recursive_mutex*>(mutex)->lock();
}

void Mutex_Unlock(Mutex* mutex) {
    reinterpret_cast<std::recursive_mutex*>(mutex)->unlock();
}

bool Mutex_TryLock(Mutex* mutex) {
    return reinterpret_cast<std::recursive_mutex*>(mutex)->try_lock();
}

void Sleep(u64 usecs) {
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

u64 GetUSCount() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

// --- Save/firmware/RTC write-back — no-ops. MelonDSCore persists NDS
// save data itself in Shutdown() by reading GetNDSSave() directly,
// rather than relying on this callback (see melon_ds_core.cpp). ---

void WriteNDSSave(const u8* /*savedata*/, u32 /*savelen*/, u32 /*writeoffset*/, u32 /*writelen*/,
                   void* /*userdata*/) {}

void WriteGBASave(const u8* /*savedata*/, u32 /*savelen*/, u32 /*writeoffset*/, u32 /*writelen*/,
                   void* /*userdata*/) {}

void WriteFirmware(const Firmware& /*firmware*/, u32 /*writeoffset*/, u32 /*writelen*/,
                    void* /*userdata*/) {}

void WriteDateTime(int /*year*/, int /*month*/, int /*day*/, int /*hour*/, int /*minute*/,
                    int /*second*/, void* /*userdata*/) {}

// --- Local multiplayer — unsupported; no DS-mode wireless. ---

void MP_Begin(void* /*userdata*/) {}
void MP_End(void* /*userdata*/) {}
int MP_SendPacket(u8* /*data*/, int /*len*/, u64 /*timestamp*/, void* /*userdata*/) {
    return 0;
}
int MP_RecvPacket(u8* /*data*/, u64* /*timestamp*/, void* /*userdata*/) {
    return 0;
}
int MP_SendCmd(u8* /*data*/, int /*len*/, u64 /*timestamp*/, void* /*userdata*/) {
    return 0;
}
int MP_SendReply(u8* /*data*/, int /*len*/, u64 /*timestamp*/, u16 /*aid*/, void* /*userdata*/) {
    return 0;
}
int MP_SendAck(u8* /*data*/, int /*len*/, u64 /*timestamp*/, void* /*userdata*/) {
    return 0;
}
int MP_RecvHostPacket(u8* /*data*/, u64* /*timestamp*/, void* /*userdata*/) {
    return 0;
}
u16 MP_RecvReplies(u8* /*data*/, u64 /*timestamp*/, u16 /*aidmask*/, void* /*userdata*/) {
    return 0;
}

// --- Networking (DS-mode local WiFi internet access) — unsupported. ---

int Net_SendPacket(u8* /*data*/, int /*len*/, void* /*userdata*/) {
    return 0;
}
int Net_RecvPacket(u8* /*data*/, void* /*userdata*/) {
    return 0;
}

// --- DSi camera — unsupported. ---

void Camera_Start(int /*num*/, void* /*userdata*/) {}
void Camera_Stop(int /*num*/, void* /*userdata*/) {}
void Camera_CaptureFrame(int /*num*/, u32* /*frame*/, int /*width*/, int /*height*/, bool /*yuv*/,
                          void* /*userdata*/) {}

// --- Microphone — unsupported for now. ---

void Mic_Start(void* /*userdata*/) {}
void Mic_Stop(void* /*userdata*/) {}
int Mic_ReadInput(s16* /*data*/, int /*maxlength*/, void* /*userdata*/) {
    return 0;
}

// --- AAC decoding (DSi DSP HLE) — unsupported. ---

AACDecoder* AAC_Init() {
    return nullptr;
}
void AAC_DeInit(AACDecoder* /*dec*/) {}
bool AAC_Configure(AACDecoder* /*dec*/, int /*frequency*/, int /*channels*/) {
    return false;
}
bool AAC_DecodeFrame(AACDecoder* /*dec*/, const void* /*input*/, int /*inputlen*/,
                      void* /*output*/, int /*outputlen*/) {
    return false;
}

// --- Addon inputs (guitar grip, rumble pak, motion pak) — unsupported. ---

bool Addon_KeyDown(KeyType /*type*/, void* /*userdata*/) {
    return false;
}
void Addon_RumbleStart(u32 /*len*/, void* /*userdata*/) {}
void Addon_RumbleStop(void* /*userdata*/) {}
float Addon_MotionQuery(MotionQueryType /*type*/, void* /*userdata*/) {
    return 0.0f;
}

// --- Dynamic library loading — only used by melonDS's own GL loader,
// which we never build (software renderer only). ---

DynamicLibrary* DynamicLibrary_Load(const char* /*lib*/) {
    return nullptr;
}
void DynamicLibrary_Unload(DynamicLibrary* /*lib*/) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary* /*lib*/, const char* /*name*/) {
    return nullptr;
}

} // namespace melonDS::Platform
