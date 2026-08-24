#!/bin/sh -ex

mkdir build
cd build

if [ "$GITHUB_REF_TYPE" == "tag" ]; then
	export EXTRA_CMAKE_FLAGS=(-DENABLE_QT_UPDATE_CHECKER=ON)
fi

# libzip (vendored as a submodule, not through vcpkg) hard-requires a
# findable zlib, but this project has no vcpkg.json manifest and
# doesn't vendor zlib itself, so nothing on a stock Windows runner
# provides one. On MSYS2, pacboy already installs it (see
# .github/workflows/build.yml) onto the standard MinGW search paths,
# so nothing extra is needed there. On plain MSVC, install it through
# vcpkg's classic (non-manifest) mode and hint CMake at its install
# prefix, rather than wiring CMAKE_TOOLCHAIN_FILE: the toolchain file
# makes vcpkg intercept every find_package() call, and its
# x64-windows triplet only produces MSVC-ABI binaries anyway (it fails
# outright under MSYS2's Clang toolchain), and the runner's default
# package set also happens to include Zydis, which collides with this
# project's own vendored Zydis copy (externals/dynarmic/externals).
# (zstd doesn't need the same treatment — cmake/melonds.cmake already
# falls back to building it from source when no system package is
# found, on any platform.) Check $TARGET (set directly from the build
# matrix in build.yml) rather than $MSYSTEM: the msvc job's plain
# "bash" shell is itself Git Bash, which is MSYS-derived and sets
# $MSYSTEM too, so it can't tell the two jobs apart.
if [ "$TARGET" != "msys2" ]; then
	VCPKG_BIN="${VCPKG_INSTALLATION_ROOT:-$VCPKG_ROOT}"
	if [ -n "$VCPKG_BIN" ]; then
		"$VCPKG_BIN/vcpkg" install zlib:x64-windows
		VCPKG_INSTALLED="$VCPKG_BIN/installed/x64-windows"
	fi
fi

cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DENABLE_QT_TRANSLATION=ON \
    -DUSE_DISCORD_PRESENCE=ON \
    ${VCPKG_INSTALLED:+-DZLIB_ROOT="$VCPKG_INSTALLED"} \
    ${VCPKG_INSTALLED:+-DCMAKE_PREFIX_PATH="$VCPKG_INSTALLED"} \
	"${EXTRA_CMAKE_FLAGS[@]}"
ninja
ninja bundle
strip -s bundle/*.exe

ccache -s -v

ctest -VV -C Release || echo "::error ::Test error occurred on Windows build"
