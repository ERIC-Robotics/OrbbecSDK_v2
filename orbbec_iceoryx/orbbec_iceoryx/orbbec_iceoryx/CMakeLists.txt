cmake_minimum_required(VERSION 3.16)
project(orbbec_iceoryx)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

include(cmake/dependencies.cmake)

# ── Iceoryx ────────────────────────────────────────────────────────────────
if(NOT DEFINED CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_PREFIX_PATH "/usr/local")
endif()

find_package(iceoryx_platform 2.95.8 REQUIRED)
find_package(iceoryx_hoofs    2.95.8 REQUIRED)
find_package(iceoryx_posh     2.95.8 REQUIRED)

message(STATUS "orbbec_iceoryx: using iceoryx ${iceoryx_posh_VERSION}")

include(IceoryxPackageHelper)
include(IceoryxPlatform)
include(IceoryxPlatformSettings)

# ── Other dependencies ─────────────────────────────────────────────────────
find_package(fmt       REQUIRED)
find_package(yaml-cpp  REQUIRED)
find_package(OpenCV    REQUIRED COMPONENTS core imgcodecs imgproc highgui videoio)

# GStreamer (optional — needed for orbbec_gst_publisher)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GST QUIET
        gstreamer-1.0>=1.18
        gstreamer-app-1.0>=1.18)
    if(GST_FOUND)
        message(STATUS "orbbec_iceoryx: GStreamer ${GST_gstreamer-1.0_VERSION} found — building orbbec_gst_publisher")
    else()
        message(STATUS "orbbec_iceoryx: GStreamer NOT found — skipping orbbec_gst_publisher")
    endif()
endif()

# ── Common includes ────────────────────────────────────────────────────────
include_directories(include)

# ── Copy config files next to binaries at build time ──────────────────────
file(COPY config/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/config)

# ── orbbec_mcap_writer (static lib) ───────────────────────────────────────
add_library(orbbec_mcap_writer STATIC src/mcap_writer.cpp)
target_include_directories(orbbec_mcap_writer PUBLIC include)
target_link_libraries(orbbec_mcap_writer PUBLIC mcap lz4 zstd)

# ── orbbec_publisher (V4L2 -> iceoryx raw frames) ─────────────────────────
iox_add_executable(
    TARGET  orbbec_publisher
    FILES   src/orbbec_publisher.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_publisher
    fmt::fmt
    yaml-cpp
    pthread
)

# ── orbbec_mock_publisher (synthetic frames, no camera required) ───────────
iox_add_executable(
    TARGET  orbbec_mock_publisher
    FILES   src/orbbec_mock_publisher.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_mock_publisher
    fmt::fmt
    yaml-cpp
    pthread
)

# ── orbbec_saver (iceoryx raw frames -> MCAP) ─────────────────────────────
iox_add_executable(
    TARGET  orbbec_saver
    FILES   src/orbbec_saver.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_saver
    orbbec_mcap_writer
    yaml-cpp
    fmt::fmt
    pthread
    stdc++fs
)

# ── orbbec_video_mcap_writer (static lib) ─────────────────────────────────
add_library(orbbec_video_mcap_writer STATIC src/video_mcap_writer.cpp)
target_include_directories(orbbec_video_mcap_writer PUBLIC include)
target_link_libraries(orbbec_video_mcap_writer PUBLIC mcap lz4 zstd)

# ── orbbec_saver_gst (iceoryx MJPEG -> MCAP foxglove.CompressedImage) ─────
iox_add_executable(
    TARGET  orbbec_saver_gst
    FILES   src/orbbec_saver_gst.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_saver_gst
    orbbec_video_mcap_writer
    fmt::fmt
    yaml-cpp
    pthread
    stdc++fs
)

# ── orbbec_monitor (health dashboard) ─────────────────────────────────────
iox_add_executable(
    TARGET  orbbec_monitor
    FILES   src/orbbec_monitor.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_monitor
    fmt::fmt
    yaml-cpp
    pthread
)

# ── orbbec_gst_publisher (V4L2 -> GStreamer MJPEG -> iceoryx) ─────────────
if(GST_FOUND)
    iox_add_executable(
        TARGET  orbbec_gst_publisher
        FILES   src/orbbec_gst_publisher.cpp
        LIBS    iceoryx_posh::iceoryx_posh
    )
    target_include_directories(orbbec_gst_publisher
        PRIVATE include ${GST_INCLUDE_DIRS})
    target_compile_options(orbbec_gst_publisher PRIVATE ${GST_CFLAGS_OTHER})
    target_link_directories(orbbec_gst_publisher PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(orbbec_gst_publisher
        ${GST_LIBRARIES}
        fmt::fmt
        pthread
    )
endif()

# ── orbbec_viewer ──────────────────────────────────────────────────────────
iox_add_executable(
    TARGET  orbbec_viewer
    FILES   scripts/orbbec_viewer.cpp
    LIBS    iceoryx_posh::iceoryx_posh
)
target_link_libraries(orbbec_viewer
    opencv_core
    opencv_imgcodecs
    opencv_imgproc
    opencv_highgui
    opencv_videoio
    fmt::fmt
    pthread
)

# ── Install ────────────────────────────────────────────────────────────────
install(DIRECTORY config/ DESTINATION lib/${PROJECT_NAME}/config)
