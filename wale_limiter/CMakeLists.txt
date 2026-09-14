cmake_minimum_required(VERSION 3.20)

# Addon version. Single source of truth: the git tag vX.Y.Z, which CI passes as
# -DWALE_VERSION=X.Y.Z. Untagged/local builds fall back to 0.0.0. This flows into the
# DLL's VERSIONINFO resource (so Windows file properties show a version) and into the
# overlay's "Version X.Y.Z" label via WALE_VERSION_STR.
set(WALE_VERSION "0.0.0" CACHE STRING "wale_limiter version (X.Y.Z)")

# Repository holding this build's source. Opened by the overlay's "View Source on GitHub"
# button, and GPLv3 requires it to be the real one. CI passes its own repository URL.
set(WALE_SOURCE_URL "https://github.com/yslwale/wale_limiter" CACHE STRING "URL of this build's source code")

# Link the CRT statically. This DLL is injected into someone else's game process, so a
# dependency on the VC++ redist would make it fail to load on PCs that lack it - which
# looks exactly like "the addon is broken" to a player. Must be set before project().
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows x64 only. Checked against the HOST before project(), because project() enables
# the RC language and would fail first with a generic "no CMAKE_RC_COMPILER" error
# elsewhere, burying this explanation.
if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "wale_limiter is a Windows x64 ReShade addon and only builds on Windows.")
endif()

project(wale_limiter VERSION ${WALE_VERSION} LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Comma-separated form (a.b.c.0) for the binary FILEVERSION/PRODUCTVERSION fields of the
# VERSIONINFO resource, generated from version.rc.in.
set(WALE_VERSION_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

add_library(wale_limiter SHARED src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

# Expose the version string (overlay label) and source URL (overlay button) to the C++ source.
target_compile_definitions(wale_limiter PRIVATE
    WALE_VERSION_STR="${PROJECT_VERSION}"
    WALE_SOURCE_URL="${WALE_SOURCE_URL}")

# The source is UTF-8 (the Hungarian UI strings); without this MSVC reads it in the system code page.
target_compile_options(wale_limiter PRIVATE /utf-8)

# ReShade addon SDK headers (v6.1.0, API 11) + the matching Dear ImGui declarations (1.90.4 docking).
target_include_directories(wale_limiter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")

# winmm        -> timeBeginPeriod/timeEndPeriod
# shell32      -> ShellExecuteA (Discord / source links)
# windowscodecs + ole32 -> WIC (decode the embedded wale-limiter-logo.png)
target_link_libraries(wale_limiter PRIVATE winmm shell32 windowscodecs ole32)

# Output the addon with ReShade's expected extension: wale_limiter.addon64
set_target_properties(wale_limiter PROPERTIES
    OUTPUT_NAME "wale_limiter"
    SUFFIX ".addon64"
    PREFIX "")
