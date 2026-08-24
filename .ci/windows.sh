#!/bin/sh -ex

mkdir build
cd build

if [ "$GITHUB_REF_TYPE" == "tag" ]; then
	export EXTRA_CMAKE_FLAGS=(-DENABLE_QT_UPDATE_CHECKER=ON)
fi

if [ -n "$VCPKG_INSTALLATION_ROOT" ]; then
	VCPKG_TOOLCHAIN="$VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
elif [ -n "$VCPKG_ROOT" ]; then
	VCPKG_TOOLCHAIN="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi

cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DENABLE_QT_TRANSLATION=ON \
    -DUSE_DISCORD_PRESENCE=ON \
    ${VCPKG_TOOLCHAIN:+-DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN"} \
	"${EXTRA_CMAKE_FLAGS[@]}"
ninja
ninja bundle
strip -s bundle/*.exe

ccache -s -v

ctest -VV -C Release || echo "::error ::Test error occurred on Windows build"
