set(CMAKE_AUTOMOC ON)

add_executable(ndsbrewer_meta
    main.cpp
    martini_progress_widget.cpp
    martini_progress_widget.h
    ndsbrewer_window.cpp
    ndsbrewer_window.h
)

set_target_properties(ndsbrewer_meta PROPERTIES OUTPUT_NAME "SweepDSEmuNDSBrewer")

if (APPLE)
    set(DIST_DIR "../../dist/apple")
    set(APPLE_RESOURCES
        "${DIST_DIR}/ndsbrewer.icns"
    )
    target_sources(ndsbrewer_meta PRIVATE ${APPLE_RESOURCES})

    # Reuses the main app's Info.plist.in template -- it's fully
    # parameterized by the MACOSX_BUNDLE_* properties set below (no
    # hardcoded "Azahar"/"SweepDS Emu" strings baked in), so there's
    # nothing citra_meta-specific to fork here. The icon itself
    # (ndsbrewer.icns) is this tool's own -- the two skewered spheres mark
    # from the SweepDS Emu logo, without the wordmark (illegible at the
    # small sizes .icns needs down to 16x16).
    include(GenerateBuildInfo)
    set(SRC_DIR "${PROJECT_SOURCE_DIR}")
    generate_build_info()
    set_target_properties(ndsbrewer_meta PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${DIST_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "SweepDSEmuNDSBrewer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.azahar-emu.ndsbrewer"
        MACOSX_BUNDLE_BUNDLE_VERSION "${BUILD_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${BUILD_FULLNAME}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${BUILD_FULLNAME}"
        MACOSX_BUNDLE_ICON_FILE "ndsbrewer.icns"
        RESOURCE "${APPLE_RESOURCES}"
    )
endif()

target_link_libraries(ndsbrewer_meta PRIVATE citra_common citra_core cryptopp fmt)

if (ENABLE_QT)
    target_link_libraries(ndsbrewer_meta PRIVATE citra_qt)
    target_link_libraries(ndsbrewer_meta PRIVATE Boost::boost Qt6::Widgets)
    # For the martini-glass loading widget (martini_progress_widget.*) --
    # QSvgWidget specifically lives in SvgWidgets, a separate Qt6 module
    # from plain Svg (QSvgRenderer/QSvgGenerator). Both are confirmed
    # available via Homebrew's qtsvg formula. Unlike QtPdf/QtVirtualKeyboard
    # (which macdeployqt separately warns about being unable to resolve --
    # unrelated optional plugin-scan noise, not something this target
    # references at all), explicitly linking these gives macdeployqt a
    # real, resolvable reason to bundle them.
    find_package(Qt6 REQUIRED COMPONENTS Svg SvgWidgets)
    target_link_libraries(ndsbrewer_meta PRIVATE Qt6::Svg Qt6::SvgWidgets)
endif()

if (ENABLE_QT AND USE_DISCORD_PRESENCE)
    target_link_libraries(ndsbrewer_meta PRIVATE discord-rpc)
endif()

if (ENABLE_QT AND UNIX AND NOT APPLE)
    target_link_libraries(ndsbrewer_meta PRIVATE Qt6::DBus gamemode)
endif()

if (ENABLE_QT AND APPLE)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
    target_link_libraries(ndsbrewer_meta PRIVATE Qt6::GuiPrivate)
endif()

if (WIN32)
    # Compile as a GUI application instead of a console application --
    # same rationale as citra_meta's identical block.
    if (MSVC)
        set_target_properties(ndsbrewer_meta PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    elseif (MINGW)
        set_target_properties(ndsbrewer_meta PROPERTIES LINK_FLAGS_RELEASE "-mwindows")
    endif()
endif()

if (SSE42_COMPILE_OPTION)
    target_compile_definitions(ndsbrewer_meta PRIVATE CITRA_HAS_SSE42)
endif()

# Bundle in-place on MSVC so dependencies can be resolved by builds --
# same rationale as citra_meta's identical block.
if (ENABLE_QT AND MSVC)
    include(BundleTarget)
    qt_bundle_target_in_place(ndsbrewer_meta)
endif()

if (UNIX AND NOT APPLE)
    install(TARGETS ndsbrewer_meta RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()

# bannertool/makerom (vendored under externals/ds_forwarder_tools/, MIT
# licensed -- see the LICENSE-*.txt files alongside them) get copied next
# to the built executable rather than linked, so NDSBrewer can shell out
# to them via QProcess at runtime.
#
# On Apple, this has to land under Contents/Resources/, not
# Contents/MacOS/ alongside the actual binary: codesign (run by
# macdeployqt below) treats loose executables/files dropped directly into
# MacOS/ as unexpected bundle components needing their own signatures,
# and fails with "code object is not signed at all" / "code has no
# resources but signature indicates they must be present" -- Resources/
# is the standard, codesign-safe home for arbitrary bundled data a Mac
# app ships alongside itself. On Windows/Linux, where there's no such
# Resources/MacOS split, "next to the exe" (applicationDirPath() itself)
# is the natural equivalent -- see NDSBrewerWindow::ForwarderToolsDir()
# for the matching platform branch on the read side.
#
# All three desktop platforms are vendored: macos/ built locally against
# this repo's own precompiled stub (see ds_forwarder_stub.h); linux/ and
# windows/ are upstream prebuilt release binaries --
# github.com/Epicpkmn11/bannertool (v1.2.2 release zip, which bundles
# linux-x86_64/windows-x86_64/mac-x86_64 builds together) and
# github.com/3DSGuy/Project_CTR (makerom-v0.19.0 release, published per
# platform). This step is a no-op (not a build failure) on any platform
# without a matching subdirectory, so it stays harmless if a platform's
# binaries ever go missing rather than failing the whole build.
if (APPLE)
    set(NDS_FORWARDER_TOOLS_PLATFORM "macos")
    set(NDS_FORWARDER_TOOLS_DEST "$<TARGET_BUNDLE_CONTENT_DIR:ndsbrewer_meta>/Resources/ds_forwarder_tools")
elseif (WIN32)
    set(NDS_FORWARDER_TOOLS_PLATFORM "windows")
    set(NDS_FORWARDER_TOOLS_DEST "$<TARGET_FILE_DIR:ndsbrewer_meta>/ds_forwarder_tools")
else()
    set(NDS_FORWARDER_TOOLS_PLATFORM "linux")
    set(NDS_FORWARDER_TOOLS_DEST "$<TARGET_FILE_DIR:ndsbrewer_meta>/ds_forwarder_tools")
endif()
set(NDS_FORWARDER_TOOLS_SRC
    "${PROJECT_SOURCE_DIR}/externals/ds_forwarder_tools/${NDS_FORWARDER_TOOLS_PLATFORM}")
if (EXISTS "${NDS_FORWARDER_TOOLS_SRC}")
    add_custom_command(TARGET ndsbrewer_meta POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${NDS_FORWARDER_TOOLS_DEST}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${NDS_FORWARDER_TOOLS_SRC}"
            "${NDS_FORWARDER_TOOLS_DEST}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${PROJECT_SOURCE_DIR}/externals/ds_forwarder_tools/shared/template.rsf"
            "${NDS_FORWARDER_TOOLS_DEST}/template.rsf"
    )
endif()
