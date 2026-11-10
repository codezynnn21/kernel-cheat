cmake_minimum_required(VERSION 3.22.1)

# must be set before project(): the NDK toolchain resolves the STL during project().
# self-contained payload: no DT_NEEDED on libc++_shared.so in a dlopen'd library
set(ANDROID_STL c++_static CACHE STRING "" FORCE)

project(codm_kit LANGUAGES C CXX)

# CODM overlay kit — arm64-v8a only, Android NDK toolchain.
# Produces:
#   codm-inject   root daemon, ptrace-injects the payload into CODM
#   libcodm.so    in-process payload: il2cpp ESP + ImGui GLES3 overlay

if(NOT ANDROID)
    message(FATAL_ERROR "This project must be configured with the Android NDK toolchain "
                        "(-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake).")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "CODM ships arm64-v8a only; configure with -DANDROID_ABI=arm64-v8a.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

if(ANDROID_PLATFORM)
    string(REGEX MATCH "([0-9]+)$" _dummy "${ANDROID_PLATFORM}")
    if(CMAKE_MATCH_1 AND CMAKE_MATCH_1 LESS 24)
        message(FATAL_ERROR "API 24+ required (PTRACE_GETREGSET, process_vm_*)")
    endif()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- Dear ImGui 1.92.6 — same generation as the reference binary -------------
set(IMGUI_TAG "v1.92.6" CACHE STRING "Dear ImGui tag")
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_TAG}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_OPENGL_ES3
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE -w)

# ---- ShadowHook 1.0.10 — compiled straight into the payload -------------------
# Static, not a shared shadowhook.so: dlopen("/data/adb/.../libcodm.so") cannot
# resolve a DT_NEEDED entry from a path the linker namespace does not own.
set(SHADOWHOOK_TAG "v1.0.10" CACHE STRING "ShadowHook tag")
FetchContent_Declare(shadowhook
    URL https://github.com/bytedance/android-inline-hook/archive/refs/tags/${SHADOWHOOK_TAG}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(shadowhook)

set(SH_DIR ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp)
if(NOT EXISTS ${SH_DIR}/shadowhook.c)
    message(FATAL_ERROR "shadowhook sources not found at ${SH_DIR}")
endif()

file(GLOB SH_SRC
    ${SH_DIR}/*.c
    ${SH_DIR}/arch/arm64/*.c
    ${SH_DIR}/common/*.c
    ${SH_DIR}/third_party/*/*.c)

add_library(shadowhook_static STATIC ${SH_SRC})
target_include_directories(shadowhook_static PUBLIC ${SH_DIR}/include)
target_include_directories(shadowhook_static PRIVATE
    ${SH_DIR}
    ${SH_DIR}/arch/arm64
    ${SH_DIR}/common
    ${SH_DIR}/third_party/xdl
    ${SH_DIR}/third_party/bsd
    ${SH_DIR}/third_party/lss)
target_compile_options(shadowhook_static PRIVATE -Os -w -ffunction-sections -fdata-sections)
target_link_libraries(shadowhook_static PUBLIC log)

# ---- root daemon -------------------------------------------------------------
add_executable(codm-inject
    native/injector.cpp
    native/remote.cpp)
target_include_directories(codm-inject PRIVATE native)
target_compile_options(codm-inject PRIVATE -Wall -Wextra -Werror -O2)
target_compile_definitions(codm-inject PRIVATE _GNU_SOURCE)
target_link_options(codm-inject PRIVATE -s)
target_link_libraries(codm-inject PRIVATE log dl)
set_target_properties(codm-inject PROPERTIES OUTPUT_NAME "codm-inject")

# ---- in-process payload ------------------------------------------------------
add_library(codm SHARED
    native/payload/entry.cpp
    native/payload/il2cpp.cpp
    native/payload/esp.cpp)
target_include_directories(codm PRIVATE native native/payload)
target_compile_options(codm PRIVATE -Wall -Wextra -Werror -O2 -fvisibility=hidden)
target_compile_definitions(codm PRIVATE _GNU_SOURCE)
target_link_libraries(codm PRIVATE
    imgui
    shadowhook_static
    android
    EGL
    GLESv3
    log
    dl)
# 16 KB page support (Android 15+ boot images); harmless on 4 KB devices.
# exclude-libs keeps xdl/shadowhook symbols out of the payload's dynsym, so they
# cannot interpose over the loader's own copies.
target_link_options(codm PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -s)
set_target_properties(codm PROPERTIES OUTPUT_NAME "codm")
