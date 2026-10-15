cmake_minimum_required(VERSION 3.25)
# Release version (semver, may carry a -alpha.N/-beta.N/-rc.N suffix). Bumped by
# .github/scripts/bump-version.sh together with packaging/; project() only takes the numeric part.
set(BLINK_VERSION 0.1.1-alpha.2)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" BLINK_VERSION_NUMERIC "${BLINK_VERSION}")
# C for the Wayland protocol glue that wayland-scanner generates.
project(blink VERSION ${BLINK_VERSION_NUMERIC} DESCRIPTION "Screenshots and screen recordings for KDE Plasma" LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json, which clangd finds

include(GNUInstallDirs)
include(CheckIPOSupported)
check_ipo_supported(RESULT BLINK_IPO LANGUAGES CXX)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets DBus Network WaylandClient)
find_package(KF6GlobalAccel 6.0 REQUIRED)
find_package(KF6GuiAddons 6.0 REQUIRED)
find_package(LayerShellQt 6.0 REQUIRED)
# 1.21 added the PipeWire object serial to zkde_screencast_stream_unstable_v1.
find_package(PlasmaWaylandProtocols 1.21 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Wayland REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3)
# FFmpeg 7.1 or newer: avcodec_get_supported_config replaced AVCodec::pix_fmts.
pkg_check_modules(FFmpeg REQUIRED IMPORTED_TARGET libavcodec>=61.19 libavformat libavfilter libavutil libswresample libswscale)
qt_standard_project_setup()

add_compile_options(-Wall -Wextra -Wpedantic)

set(BLINK_RECORDER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/blink/blink-recorder")

# Screen recorder, run by the tray app for each recording: plain C++, no Qt. PipeWire in, FFmpeg out.
add_executable(blink-recorder
    recorder/audioencoder.cpp recorder/audioencoder.h
    recorder/capture.cpp recorder/capture.h
    recorder/events.cpp recorder/events.h
    recorder/ffmpeg.cpp recorder/ffmpeg.h
    recorder/framequeue.cpp recorder/framequeue.h
    recorder/main.cpp
    recorder/mixer.cpp recorder/mixer.h
    recorder/muxer.cpp recorder/muxer.h
    recorder/thumbnail.cpp recorder/thumbnail.h
    recorder/videoencoder.cpp recorder/videoencoder.h
    src/recorderprotocol.h
)
target_include_directories(blink-recorder PRIVATE src)
target_link_libraries(blink-recorder PRIVATE PkgConfig::PipeWire PkgConfig::FFmpeg Threads::Threads)
target_link_options(blink-recorder PRIVATE -Wl,--as-needed)

qt_add_executable(blink
    src/app.cpp src/app.h
    src/appname.cpp src/appname.h
    src/clipboard.cpp src/clipboard.h
    src/kwinscreenshot.cpp src/kwinscreenshot.h
    src/librarymodel.cpp src/librarymodel.h
    src/librarywindow.cpp src/librarywindow.h
    src/main.cpp
    src/notifier.cpp src/notifier.h
    src/pathpattern.cpp src/pathpattern.h
    src/pngencoder.cpp src/pngencoder.h
    src/recorderprotocol.h
    src/recording.cpp src/recording.h
    src/regionselector.cpp src/regionselector.h
    src/screencast.cpp src/screencast.h
    src/settings.cpp src/settings.h
    src/settingsdialog.cpp src/settingsdialog.h
    src/thumbnailer.cpp src/thumbnailer.h
    src/uploader.cpp src/uploader.h
    src/windowlist.cpp src/windowlist.h
)
if(QT_KNOWN_POLICY_QTP0006) # Qt 6.12+: keep the generated protocol symbols internal
    qt_policy(SET QTP0006 NEW)
endif()
qt6_generate_wayland_protocol_client_sources(blink FILES ${PLASMA_WAYLAND_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml)
qt_add_resources(blink resources PREFIX / BASE data FILES data/windowlist.js data/icons/hicolor/scalable/apps/blink.svg)
target_link_libraries(blink PRIVATE
    Qt6::Widgets Qt6::DBus Qt6::Network Qt6::WaylandClient
    KF6::GlobalAccel KF6::GuiAddons LayerShellQt::Interface
    PkgConfig::Wayland ZLIB::ZLIB
)
target_link_options(blink PRIVATE -Wl,--as-needed)
target_compile_definitions(blink PRIVATE
    BLINK_VERSION="${BLINK_VERSION}"
    BLINK_RECORDER_PATH="${BLINK_RECORDER_PATH}"
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_KEYWORDS
    QT_NO_FOREACH
    QT_USE_QSTRINGBUILDER
)

if(BLINK_IPO)
    set_target_properties(blink blink-recorder PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

install(TARGETS blink RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS blink-recorder RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/blink)
configure_file(data/blink.desktop.in blink.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/blink.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY data/icons/hicolor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
