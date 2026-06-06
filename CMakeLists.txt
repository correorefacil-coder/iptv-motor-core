cmake_minimum_required(VERSION 3.12)
project(AntigravityIPTV CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find pkg-config to locate FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Threads support
find_package(Threads REQUIRED)

# Source files
add_executable(antigravity_iptv
    src/main.cpp
    src/logger.cpp
    src/engine.cpp
    src/http_server.cpp
)

# Header search paths
target_include_directories(antigravity_iptv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

# Library linking
target_link_libraries(antigravity_iptv PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    Threads::Threads
)
