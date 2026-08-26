#!/bin/bash -ex

if [[ "$TARGET" == "appimage"* ]]; then
    # Compile the AppImage we distribute with Clang.
    export EXTRA_CMAKE_FLAGS=(-DCMAKE_CXX_COMPILER=clang++
                              -DCMAKE_C_COMPILER=clang
                              -DCMAKE_LINKER=/etc/bin/ld.lld
                              -DENABLE_ROOM_STANDALONE=OFF)
    if [ "$TARGET" = "appimage-wayland" ]; then
        # Bundle required QT wayland libraries
        export EXTRA_QT_PLUGINS="waylandcompositor"
        export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
    fi
else
    # For the linux-fresh verification target, verify compilation without PCH as well.
    export EXTRA_CMAKE_FLAGS=(-DCITRA_USE_PRECOMPILED_HEADERS=OFF)
fi

if [ "$GITHUB_REF_TYPE" == "tag" ]; then
    export EXTRA_CMAKE_FLAGS=("${EXTRA_CMAKE_FLAGS[@]}" -DENABLE_QT_UPDATE_CHECKER=ON)
fi

mkdir build
cd build
cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DENABLE_QT_TRANSLATION=ON \
    -DENABLE_ROOM_STANDALONE=OFF \
    -DUSE_DISCORD_PRESENCE=ON \
    "${EXTRA_CMAKE_FLAGS[@]}"
ninja
# -maxdepth 1 -type f rather than a bare "bin/Release/*" glob: strip
# can't process a directory argument (exits nonzero, aborting this
# script under set -e) and bin/Release/ isn't guaranteed to contain
# only binaries -- ndsbrewer_meta's CMakeLists.txt copies its
# ds_forwarder_tools/ helper directory in next to the built
# executables there.
find bin/Release -maxdepth 1 -type f -exec strip -s {} +

if [[ "$TARGET" == "appimage"* ]]; then
    ninja bundle
    # TODO: Our AppImage environment currently uses an older ccache version without the verbose flag.
    ccache -s
else
    ccache -s -v
fi

ctest -VV -C Release
