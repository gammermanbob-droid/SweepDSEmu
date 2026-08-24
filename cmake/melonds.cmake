# cmake/melonds.cmake
#
# Include this from Azahar's top-level CMakeLists.txt with:
#   include(cmake/melonds.cmake)
#
# It vendors melonDS as a subdirectory and builds *only* its core
# (src/*.cpp, NOT src/frontend/qt_sdl/*) as a static library, so we
# don't pull in a second Qt/SDL event loop — Azahar's own frontend
# stays the single UI owner and just calls into this lib per-frame.

include(FetchContent)

FetchContent_Declare(
    melonds
    GIT_REPOSITORY https://github.com/melonDS-emu/melonDS.git
    GIT_TAG        master   # pin to a specific commit/tag before shipping
)

# Populate() only clones the source tree without executing melonDS's
# own CMakeLists.txt, which would otherwise define its own `teakra`
# target (colliding with Azahar's existing one) and pull in SDL2/
# Vulkan requirements from melonDS's Qt frontend that we never build.
FetchContent_GetProperties(melonds)
if(NOT melonds_POPULATED)
    FetchContent_Populate(melonds)
endif()

# melonDS's FATStorage.cpp calls std::filesystem::path::u8string() and
# assigns/constructs a std::string directly from the result. That returns
# std::u8string (char8_t-based) as of C++20, which doesn't implicitly
# convert to std::string under libc++'s stricter C++20 std::string —
# breaks the build on macOS Clang. This has to be patched at configure
# time (rather than by hand) because FetchContent_Populate re-clones a
# fresh, unpatched copy of melonDS every time build/ is wiped.
set(MELONDS_FATSTORAGE_CPP "${melonds_SOURCE_DIR}/src/FATStorage.cpp")
if(EXISTS "${MELONDS_FATSTORAGE_CPP}")
    file(READ "${MELONDS_FATSTORAGE_CPP}" MELONDS_FATSTORAGE_CONTENTS)
    if(NOT MELONDS_FATSTORAGE_CONTENTS MATCHES "U8StringToString")
        string(REPLACE
            "using std::string;\n"
            "using std::string;\n\n// std::filesystem::path::u8string() returns std::u8string (char8_t-based) as of C++20,\n// which does not implicitly convert to std::string. This converts it explicitly.\nstatic std::string U8StringToString(const std::u8string& u8str)\n{\n    return std::string(u8str.begin(), u8str.end());\n}\n"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        string(REPLACE
            "OpenFile(out.u8string(), FileMode::Write)"
            "OpenFile(U8StringToString(out.u8string()), FileMode::Write)"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        string(REPLACE
            "Platform::OpenFile(in.u8string(), FileMode::Read)"
            "Platform::OpenFile(U8StringToString(in.u8string()), FileMode::Read)"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        string(REPLACE
            "std::string fullpath = entry.path().u8string();"
            "std::string fullpath = U8StringToString(entry.path().u8string());"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        file(WRITE "${MELONDS_FATSTORAGE_CPP}" "${MELONDS_FATSTORAGE_CONTENTS}")
    endif()

    # ImportDirectory()/GetDirectorySize() walk SourceDir with a plain
    # recursive_directory_iterator, which by default does NOT descend
    # into symlinked directories. We point a homebrew ROM's SD card
    # SourceDir at a small directory of symlinks (see melon_ds_core.cpp)
    # rather than a real folder, so it can expose just a couple of real
    # subfolders (e.g. sdmc/_nds, sdmc/roms) without importing
    # everything else alongside them — without this, both calls would
    # treat those symlinks as empty leaf directories and never actually
    # walk into the real data. Guarded independently from the
    # u8string patch above since they can each land or not land
    # independently across reconfigures.
    file(READ "${MELONDS_FATSTORAGE_CPP}" MELONDS_FATSTORAGE_CONTENTS)
    if(NOT MELONDS_FATSTORAGE_CONTENTS MATCHES "follow_directory_symlink")
        string(REPLACE
            "for (auto& entry : fs::recursive_directory_iterator(fs::u8path(sourcedir)))"
            "for (auto& entry : fs::recursive_directory_iterator(fs::u8path(sourcedir), fs::directory_options::follow_directory_symlink))"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        string(REPLACE
            "for (auto& entry : fs::recursive_directory_iterator(sourcedir))"
            "for (auto& entry : fs::recursive_directory_iterator(sourcedir, fs::directory_options::follow_directory_symlink))"
            MELONDS_FATSTORAGE_CONTENTS "${MELONDS_FATSTORAGE_CONTENTS}")
        file(WRITE "${MELONDS_FATSTORAGE_CPP}" "${MELONDS_FATSTORAGE_CONTENTS}")
    endif()
endif()

# melonDS's DSi_DSP.cpp falls back to LLE-emulating the DSi's actual
# Teak DSP via its own vendored copy of the "teakra" library
# (src/teakra/) when it doesn't recognize a title's HLE ucode. Azahar
# already links its own (differently-versioned, 3DS-DSP-specific)
# `teakra` target — building melonDS's vendored copy too would double-
# define the same `Teakra::Teakra` class in the same process, and its
# API has drifted enough (SetMicEnableCallback/SetSharedMemoryCallback
# don't exist in Azahar's fork) that pointing melonDS at Azahar's
# teakra instead isn't an option either. We don't run DSi mode at all
# right now (see NDSArgs-only construction in melon_ds_core.cpp), so
# stub the one function that pulls Teakra in — every other DSi_DSP
# (HLE ucode) code path is untouched.
set(MELONDS_DSI_DSP_CPP "${melonds_SOURCE_DIR}/src/DSi_DSP.cpp")
if(EXISTS "${MELONDS_DSI_DSP_CPP}")
    file(READ "${MELONDS_DSI_DSP_CPP}" MELONDS_DSI_DSP_CONTENTS)
    if(NOT MELONDS_DSI_DSP_CONTENTS MATCHES "DSP LLE unavailable in this build")
        string(REPLACE
"void DSi_DSP::StartDSPLLE()
{
    auto teakra = new Teakra::Teakra();
    DSPCore = teakra;

    using namespace std::placeholders;

    teakra->SetRecvDataHandler(0, std::bind(&DSi_DSP::IrqRep0, this));
    teakra->SetRecvDataHandler(1, std::bind(&DSi_DSP::IrqRep1, this));
    teakra->SetRecvDataHandler(2, std::bind(&DSi_DSP::IrqRep2, this));

    teakra->SetSemaphoreHandler(std::bind(&DSi_DSP::IrqSem, this));

    Teakra::SharedMemoryCallback smcb;
    smcb.read16 = std::bind(&DSi_DSP::DSPRead16, this, _1);
    smcb.write16 = std::bind(&DSi_DSP::DSPWrite16, this, _1, _2);
    teakra->SetSharedMemoryCallback(smcb);

    // these happen instantaneously and without too much regard for bus aribtration
    // rules, so, this might have to be changed later on
    Teakra::AHBMCallback cb;
    cb.read8 = [this](auto addr) { return DSi.ARM9Read8(addr); };
    cb.write8 = [this](auto addr, auto val) { DSi.ARM9Write8(addr, val); };
    cb.read16 = [this](auto addr) { return DSi.ARM9Read16(addr); };
    cb.write16 = [this](auto addr, auto val) { DSi.ARM9Write16(addr, val); };
    cb.read32 = [this](auto addr) { return DSi.ARM9Read32(addr); };
    cb.write32 = [this](auto addr, auto val) { DSi.ARM9Write32(addr, val); };
    teakra->SetAHBMCallback(cb);

    teakra->SetMicEnableCallback([this](bool enable)
     {
         if (enable)
             DSi.Mic.Start(Mic_DSi_DSP);
         else
             DSi.Mic.Stop(Mic_DSi_DSP);
     });
}"
"void DSi_DSP::StartDSPLLE()
{
    // DSP LLE unavailable in this build (see cmake/melonds.cmake) — leaves
    // DSPCore null, same as its pre-init state before any ucode is picked.
    Platform::Log(Platform::LogLevel::Warn, \"DSi_DSP: LLE unavailable in this build, cannot start Teakra core\\n\");
}"
            MELONDS_DSI_DSP_CONTENTS "${MELONDS_DSI_DSP_CONTENTS}")
        file(WRITE "${MELONDS_DSI_DSP_CPP}" "${MELONDS_DSI_DSP_CONTENTS}")
    endif()
endif()

# melonDS's fastmem system installs a SIGSEGV/SIGBUS handler
# (ARMJIT_Memory::SigsegvHandler) that resolves deliberate faults from
# touching its JIT memory arena. That handler indexes through the
# thread_local NDS::Current pointer — which melonDS only ever sets at
# the top of NDS::RunFrame(), meaning it's still null for any fault
# that happens before a game's first frame runs. In our embedding, one
# does: constructing melonDS::ARMJIT (a member of the NDS being built)
# faults during its own setup, before NDS::Current is ever assigned,
# so the handler dereferences null NDS::Current — which itself faults
# *inside the SIGSEGV handler*, and since melonDS's own handler is what
# catches that second fault too, it re-enters itself forever. Guard the
# null case so it falls through to the existing "chain to the previous
# handler" path instead of hanging the process indefinitely.
set(MELONDS_ARMJIT_MEMORY_CPP "${melonds_SOURCE_DIR}/src/ARMJIT_Memory.cpp")
if(EXISTS "${MELONDS_ARMJIT_MEMORY_CPP}")
    file(READ "${MELONDS_ARMJIT_MEMORY_CPP}" MELONDS_ARMJIT_MEMORY_CONTENTS)
    # Diagnostic: the fixed-address remap of the fastmem arena's real
    # backing memory (over the PROT_NONE reservation taken a few lines
    # above) has its return value silently discarded upstream — if it
    # fails, MemoryBase keeps pointing at the still-PROT_NONE
    # reservation, and the next write into it (zero-filling the arena)
    # segfaults with no indication of why. Log the actual errno.
    if(NOT MELONDS_ARMJIT_MEMORY_CONTENTS MATCHES "mmap remap of fastmem arena failed")
        string(REPLACE
            "    mmap(MemoryBase, MemoryTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, MemoryFile, 0);\n#endif"
            "    void* mmapRemapResult = mmap(MemoryBase, MemoryTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, MemoryFile, 0);\n    if (mmapRemapResult == MAP_FAILED)\n    {\n        Log(LogLevel::Error, \"mmap remap of fastmem arena failed! base=%p size=%llu errno=%d (%s)\\n\", MemoryBase, (unsigned long long)MemoryTotalSize, errno, strerror(errno));\n    }\n#endif"
            MELONDS_ARMJIT_MEMORY_CONTENTS "${MELONDS_ARMJIT_MEMORY_CONTENTS}")
        file(WRITE "${MELONDS_ARMJIT_MEMORY_CPP}" "${MELONDS_ARMJIT_MEMORY_CONTENTS}")
    endif()
    if(NOT MELONDS_ARMJIT_MEMORY_CONTENTS MATCHES "NDS::Current == nullptr")
        string(REPLACE
"    ucontext_t* context = (ucontext_t*)rawContext;

    FaultDescription desc {};"
"    ucontext_t* context = (ucontext_t*)rawContext;

    if (NDS::Current == nullptr)
    {
        // Faulted before the first RunFrame() ever set NDS::Current —
        // nothing to resolve against. Chain to whatever handler was
        // installed before ours rather than dereferencing null.
        struct sigaction* oldSaEarly = (sig == SIGSEGV) ? &OldSaSegv : &OldSaBus;
        if (oldSaEarly->sa_flags & SA_SIGINFO)
        {
            oldSaEarly->sa_sigaction(sig, info, rawContext);
            return;
        }
        if (oldSaEarly->sa_handler == SIG_DFL)
        {
            signal(sig, SIG_DFL);
            return;
        }
        if (oldSaEarly->sa_handler == SIG_IGN)
        {
            return;
        }
        oldSaEarly->sa_handler(sig);
        return;
    }

    FaultDescription desc {};"
            MELONDS_ARMJIT_MEMORY_CONTENTS "${MELONDS_ARMJIT_MEMORY_CONTENTS}")
        file(WRITE "${MELONDS_ARMJIT_MEMORY_CPP}" "${MELONDS_ARMJIT_MEMORY_CONTENTS}")
    endif()
endif()

# The JIT backend selector only recognizes GCC/Clang's __x86_64__,
# never MSVC's own _M_X64 — meaning it never selects a backend at all
# on MSVC and fails with "the current target platform doesn't have a
# JIT backend" despite xbyak (melonDS's x64 JIT backend) itself
# supporting MSVC just fine.
set(MELONDS_JIT_COMPILER_H "${melonds_SOURCE_DIR}/src/ARMJIT_Compiler.h")
if(EXISTS "${MELONDS_JIT_COMPILER_H}")
    file(READ "${MELONDS_JIT_COMPILER_H}" MELONDS_JIT_COMPILER_CONTENTS)
    if(NOT MELONDS_JIT_COMPILER_CONTENTS MATCHES "_M_X64")
        string(REPLACE
            "#if defined(__x86_64__)"
            "#if defined(__x86_64__) || defined(_M_X64)"
            MELONDS_JIT_COMPILER_CONTENTS "${MELONDS_JIT_COMPILER_CONTENTS}")
        file(WRITE "${MELONDS_JIT_COMPILER_H}" "${MELONDS_JIT_COMPILER_CONTENTS}")
    endif()
endif()

# melonDS's own CMakeLists builds a monolithic `melonDS` target that
# includes the Qt frontend. Build our own restricted target instead.
#
# This mirrors melonDS's own src/CMakeLists.txt `add_library(core ...)`
# file list explicitly (JIT backend + OpenGL/GDB stripped out) rather
# than glob-ing "src/*.cpp": melonDS keeps several required pieces in
# subdirectories (NDSCart/, fatfs/, blip-buf/, sha1/, tiny-AES-c/,
# xxhash/, and the JIT's dolphin/ + ARMJIT_A64|x64/ backends) that a
# flat "src/*.cpp" glob silently skips — the resulting build still
# links melonds_core.a standalone (nothing catches the gap until
# something actually calls into the core deeply enough to need those
# symbols), so this was easy to miss until then.
set(MELONDS_CORE_SOURCES
    "${melonds_SOURCE_DIR}/src/ARCodeFile.cpp"
    "${melonds_SOURCE_DIR}/src/ARDatabaseDAT.cpp"
    "${melonds_SOURCE_DIR}/src/AREngine.cpp"
    "${melonds_SOURCE_DIR}/src/ARM.cpp"
    "${melonds_SOURCE_DIR}/src/ARMInterpreter.cpp"
    "${melonds_SOURCE_DIR}/src/ARMInterpreter_ALU.cpp"
    "${melonds_SOURCE_DIR}/src/ARMInterpreter_Branch.cpp"
    "${melonds_SOURCE_DIR}/src/ARMInterpreter_LoadStore.cpp"
    "${melonds_SOURCE_DIR}/src/CP15.cpp"
    "${melonds_SOURCE_DIR}/src/CRC32.cpp"
    "${melonds_SOURCE_DIR}/src/DMA.cpp"
    "${melonds_SOURCE_DIR}/src/DMA_Timings.cpp"
    "${melonds_SOURCE_DIR}/src/DSi.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_AES.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_Camera.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_DSP.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_I2C.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_I2S.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_NAND.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_NDMA.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_NWifi.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_SD.cpp"
    "${melonds_SOURCE_DIR}/src/DSi_SPI_TSC.cpp"
    "${melonds_SOURCE_DIR}/src/FATIO.cpp"
    "${melonds_SOURCE_DIR}/src/FATStorage.cpp"
    "${melonds_SOURCE_DIR}/src/GBACart.cpp"
    "${melonds_SOURCE_DIR}/src/GBACartMotionPak.cpp"
    "${melonds_SOURCE_DIR}/src/GPU.cpp"
    "${melonds_SOURCE_DIR}/src/GPU_Soft.cpp"
    "${melonds_SOURCE_DIR}/src/GPU2D.cpp"
    "${melonds_SOURCE_DIR}/src/GPU2D_Soft.cpp"
    "${melonds_SOURCE_DIR}/src/GPU3D.cpp"
    "${melonds_SOURCE_DIR}/src/GPU3D_Soft.cpp"
    "${melonds_SOURCE_DIR}/src/GPU3D_Texcache.cpp"
    "${melonds_SOURCE_DIR}/src/Mic.cpp"
    "${melonds_SOURCE_DIR}/src/NDS.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart.cpp"
    "${melonds_SOURCE_DIR}/src/ROMList.cpp"
    "${melonds_SOURCE_DIR}/src/FreeBIOS.cpp"
    "${melonds_SOURCE_DIR}/src/RTC.cpp"
    "${melonds_SOURCE_DIR}/src/Savestate.cpp"
    "${melonds_SOURCE_DIR}/src/SPI.cpp"
    "${melonds_SOURCE_DIR}/src/SPI_Firmware.cpp"
    "${melonds_SOURCE_DIR}/src/SPU.cpp"
    "${melonds_SOURCE_DIR}/src/Utils.cpp"
    "${melonds_SOURCE_DIR}/src/Wifi.cpp"
    "${melonds_SOURCE_DIR}/src/WifiAP.cpp"

    "${melonds_SOURCE_DIR}/src/DSP_HLE/UcodeBase.cpp"
    "${melonds_SOURCE_DIR}/src/DSP_HLE/AACUcode.cpp"
    "${melonds_SOURCE_DIR}/src/DSP_HLE/G711Ucode.cpp"
    "${melonds_SOURCE_DIR}/src/DSP_HLE/GraphicsUcode.cpp"

    "${melonds_SOURCE_DIR}/src/NDSCart/CartCommon.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartRetail.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartRetailNAND.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartRetailIR.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartRetailBT.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartSD.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartHomebrew.cpp"
    "${melonds_SOURCE_DIR}/src/NDSCart/CartR4.cpp"

    "${melonds_SOURCE_DIR}/src/fatfs/ff.c"
    "${melonds_SOURCE_DIR}/src/fatfs/ffsystem.c"
    "${melonds_SOURCE_DIR}/src/fatfs/ffunicode.c"

    "${melonds_SOURCE_DIR}/src/sha1/sha1.c"
    "${melonds_SOURCE_DIR}/src/tiny-AES-c/aes.c"
    "${melonds_SOURCE_DIR}/src/xxhash/xxhash.c"

    "${melonds_SOURCE_DIR}/src/blip-buf/blip_buf.c"

    # JIT (see JIT_ENABLED below) — matches melonDS's own
    # `if (ENABLE_JIT)` block, arch-specific backend included via
    # ${ARCHITECTURE}, which Azahar's own top-level CMakeLists.txt
    # already computes (x86_64/ARM64/GENERIC) before this file is
    # include()'d.
    "${melonds_SOURCE_DIR}/src/ARM_InstrInfo.cpp"
    "${melonds_SOURCE_DIR}/src/ARMJIT.cpp"
    "${melonds_SOURCE_DIR}/src/ARMJIT_Memory.cpp"
    "${melonds_SOURCE_DIR}/src/ARMJIT_Global.cpp"
    "${melonds_SOURCE_DIR}/src/dolphin/CommonFuncs.cpp"
)

if(ARCHITECTURE STREQUAL "x86_64")
    enable_language(ASM)
    list(APPEND MELONDS_CORE_SOURCES
        "${melonds_SOURCE_DIR}/src/dolphin/x64ABI.cpp"
        "${melonds_SOURCE_DIR}/src/dolphin/x64CPUDetect.cpp"
        "${melonds_SOURCE_DIR}/src/dolphin/x64Emitter.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_x64/ARMJIT_Compiler.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_x64/ARMJIT_ALU.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_x64/ARMJIT_LoadStore.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_x64/ARMJIT_Branch.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_x64/ARMJIT_Linkage.S"
    )
elseif(ARCHITECTURE STREQUAL "arm64")
    enable_language(ASM)
    list(APPEND MELONDS_CORE_SOURCES
        "${melonds_SOURCE_DIR}/src/dolphin/Arm64Emitter.cpp"
        "${melonds_SOURCE_DIR}/src/dolphin/MathUtil.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_A64/ARMJIT_Compiler.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_A64/ARMJIT_ALU.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_A64/ARMJIT_LoadStore.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_A64/ARMJIT_Branch.cpp"
        "${melonds_SOURCE_DIR}/src/ARMJIT_A64/ARMJIT_Linkage.S"
    )
else()
    message(WARNING "melonds_core: unrecognized ARCHITECTURE '${ARCHITECTURE}' — building "
                     "without a JIT backend (interpreter-only, much slower).")
endif()

# melonDS's Platform.h is an interface the frontend implements —
# melonDS itself only ships one implementation (its Qt/SDL frontend's
# Platform.cpp), which we exclude on purpose. This is ours.
list(APPEND MELONDS_CORE_SOURCES
    "${PROJECT_SOURCE_DIR}/src/core/melonds_core/melonds_platform_headless.cpp"
)

# NDS.cpp #includes "version.h", which melonDS's own build generates
# via configure_file() from src/version.h.in. We bypass that build, so
# generate it ourselves here instead — build info embedding
# (MELONDS_EMBED_BUILD_INFO) stays off, matching melonDS's own default,
# so the git-branch/hash/provider substitutions below are unused but
# must still be defined for configure_file() not to error.
set(melonDS_VERSION "1.1")
set(melonDS_HOMEPAGE_URL "https://melonds.kuribo64.net")
set(MELONDS_VERSION_SUFFIX "")
set(MELONDS_GIT_BRANCH "")
set(MELONDS_GIT_HASH "")
set(MELONDS_BUILD_PROVIDER "")
configure_file(
    "${melonds_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/melonds_generated/version.h"
)

add_library(melonds_core STATIC ${MELONDS_CORE_SOURCES})
target_include_directories(melonds_core PUBLIC
    "${melonds_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/melonds_generated"
)

# melonDS was only ever built with GCC/Clang upstream (its own Windows
# builds go through MinGW, never MSVC), so its source leans on GNU
# extensions MSVC doesn't understand at all: __attribute__((...))
# (TinyVector.h and others) and builtins like __builtin_ctzll/
# __builtin_clzll/__builtin_popcount (NonStupidBitfield.h,
# ARMInterpreter_LoadStore.cpp, and potentially others not yet hit by
# a compile). Rather than chasing each individual file/call site as
# MSVC trips over it, force-include one small compat header into
# every translation unit in this target so all of them are covered up
# front, including any not yet discovered.
if(MSVC)
    set(MELONDS_MSVC_COMPAT_H "${CMAKE_CURRENT_BINARY_DIR}/melonds_generated/melonds_msvc_compat.h")
    file(WRITE "${MELONDS_MSVC_COMPAT_H}" "#pragma once\n\
// NOMINMAX must land before <intrin.h> drags in enough of the\n\
// Windows SDK to define max()/min() as macros, which silently mangles\n\
// every std::max/std::min call in melonDS's own source (e.g. turns\n\
// \"std::max(a, b)\" into \"std::(...)\" wherever max() gets expanded).\n\
#ifndef NOMINMAX\n\
#define NOMINMAX\n\
#endif\n\
#include <intrin.h>\n\
#ifndef __attribute__\n\
#define __attribute__(x)\n\
#endif\n\
static inline int melonds_msvc_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (int)i; }\n\
static inline int melonds_msvc_clzll(unsigned long long x) { unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i; }\n\
#define __builtin_ctzll melonds_msvc_ctzll\n\
#define __builtin_clzll melonds_msvc_clzll\n\
#define __builtin_popcount __popcnt\n\
")
    target_compile_options(melonds_core PRIVATE "/FI${MELONDS_MSVC_COMPAT_H}")
endif()

# Wipe whatever directory-scope compile definitions this target
# inherited from Azahar's top-level CMakeLists.txt (ENABLE_QT,
# ENABLE_SDL2, ENABLE_ROOM, etc. — none of which apply to melonDS's
# own source) before setting the ones melonDS's sources actually need.
set_target_properties(melonds_core PROPERTIES COMPILE_DEFINITIONS "")

target_compile_definitions(melonds_core PRIVATE
    MELONDS_HEADLESS_CORE=1
)

# JIT_ENABLED must be PUBLIC, not PRIVATE: it changes the class layout
# of NDS/ARM/ARMJIT/ARMJIT_Memory (ARMJIT.h's ARMJIT class in
# particular gains several megabytes of fixed-size array members —
# FastBlockLookup*/CodeIndex* — under this macro). Any other target
# that #includes these headers, even just to call
# std::make_unique<melonDS::NDS>(...) (e.g. citra_core via
# melon_ds_core.cpp, which only *links* against melonds_core), must
# see the exact same definition or its view of sizeof(NDS) silently
# disagrees with melonDS's own translation units — the allocator sizes
# the object for the small #else stub ARMJIT while the constructor
# actually compiled into melonds_core zero-inits the full multi-MB
# arrays, bzero'ing straight past the undersized allocation into
# unmapped memory (this was the cause of a crash inside
# ARMJIT::ARMJIT that no amount of body-level instrumentation could
# ever reach, since the fault is a member-initializer ODR mismatch,
# not a logic bug in the constructor body itself). PRIVATE compile
# definitions never propagate through target_link_libraries even when
# the link itself is PRIVATE, so this must be PUBLIC to reach
# citra_core.
target_compile_definitions(melonds_core PUBLIC
    # melonDS gates its fastmem JIT support (ARMJIT_Memory's
    # FastMem9Start/FastMem7Start/Mapping members, ARMJIT's
    # LiteralOptimizations/BranchOptimizations/RestoreCandidates)
    # behind this macro. melonDS's own build always defines it on
    # JIT-capable desktop platforms (which includes arm64 macOS);
    # we replicate that here since we bypass melonDS's own CMake
    # configuration logic.
    JIT_ENABLED
)

# melonDS core needs zstd (savestate compression). Azahar's own
# externals/CMakeLists.txt (add_subdirectory(externals), always
# processed before this file is included — see CMakeLists.txt) already
# creates a plain "zstd" INTERFACE target unconditionally, whether
# USE_SYSTEM_ZSTD finds a system package or it builds one from the
# vendored externals/zstd submodule — the same target either way, on
# every platform. Reuse it directly instead of duplicating that
# discovery/build logic here (an earlier version of this file tried to
# vendor a second copy via FetchContent when no system package was
# found, which collided with this exact target on Android).
target_link_libraries(melonds_core PUBLIC zstd)

if(APPLE)
    # melonDS's GPU code has a Metal-backed OpenGL path on macOS
    # (Apple deprecated OpenGL; melonDS renders through Apple's GL
    # shim or ANGLE depending on version). No extra linking is
    # needed here beyond the system frameworks, but they must be
    # explicit — CMake won't pull these in implicitly on macOS the
    # way it auto-links GL on Linux.
    target_link_libraries(melonds_core PUBLIC
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework IOKit"
    )
endif()
