cmake_minimum_required(VERSION 3.10)

# Bloodframe (libbloodframe.so, Android): only that target, nothing of the layer below. See android/ and
# build-android.sh.
option(BLOODFRAME "Build only Bloodframe, the single libbloodframe.so (Android NDK)" OFF)
if(BLOODFRAME)
    project(bloodframe LANGUAGES C CXX)
    add_subdirectory(android)
    return()
endif()

# Android / Wine-on-Android target (for GameNative emulator).
# When ON: builds liblsfg-vk.so for the NDK toolchain, skips the GTK4 UI,
# and defines LSFGVK_ANDROID_WINE=1 for the C++ code.
option(LSFGVK_ANDROID_WINE "Build for GameNative / Wine-on-Android" OFF)

set(CMAKE_SKIP_RPATH ON)
set(CMAKE_C_VISIBILITY_PRESET "hidden")
set(CMAKE_CXX_VISIBILITY_PRESET "hidden")

if(LSFGVK_ANDROID_WINE)
    add_compile_definitions(LSFGVK_ANDROID_WINE=1)
endif()

# subprojects
add_compile_options(-fPIC
    -Wno-deprecated-declarations
    -Wno-unused-template)

add_subdirectory(thirdparty/dxbc EXCLUDE_FROM_ALL)
add_subdirectory(thirdparty/pe-parse/pe-parser-library EXCLUDE_FROM_ALL)
add_subdirectory(thirdparty/toml11 EXCLUDE_FROM_ALL)
add_subdirectory(thirdparty/volk EXCLUDE_FROM_ALL)
add_subdirectory(framegen)

# main project
project(lsfg-vk
    VERSION 1.0.0
    DESCRIPTION "Lossless Scaling Frame Generation on Linux"
    LANGUAGES CXX)

file(GLOB SOURCES
    "src/config/*.cpp"
    "src/extract/*.cpp"
    "src/mini/*.cpp"
    "src/utils/*.cpp"
    "src/*.cpp"
)

# Android has a dedicated layer entrypoint that treats non-presenting Vulkan
# devices as pass-through. The desktop layer keeps its existing behavior.
if(ANDROID)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/layer.cpp")

    # Keep the manifest's proven production entrypoint names exactly unchanged,
    # but route this diagnostic build through a transparent provenance shim.
    # Only layer_android.cpp sees these renames; android_wsi_loader_bridge.cpp
    # exports layer_vkGet*ProcAddr and forwards to these implementation symbols.
    # This avoids changing the Android/Wine loader contract that is known to be
    # sensitive on Xclipse while still letting us see which WSI pointer is
    # returned and whether that exact trampoline is later invoked.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/layer_android.cpp"
        PROPERTIES COMPILE_DEFINITIONS
        "layer_vkGetInstanceProcAddr=lsfg_layer_vkGetInstanceProcAddr_impl;layer_vkGetDeviceProcAddr=lsfg_layer_vkGetDeviceProcAddr_impl"
    )
else()
    # Android loader/provenance entrypoints intentionally shadow the production
    # layer entrypoints on Android and must never be linked into the desktop layer.
    list(REMOVE_ITEM SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/android_diagnostics.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/android_wsi_loader_bridge.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/layer_android.cpp"
    )
endif()

add_library(lsfg-vk SHARED ${SOURCES})

# target
set_target_properties(lsfg-vk PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg-vk
    PRIVATE include)
target_link_libraries(lsfg-vk PRIVATE
    pe-parse dxbc toml11
    lsfg-vk-framegen)

if(ANDROID)
    # AHardwareBuffer_allocate/release are exported by libnativewindow (NDK API 26+).
    # Link that narrow NDK surface directly instead of libandroid.so. In GameNative's
    # Bionic guest, libandroid.so pulls Android framework dependencies through a
    # guest-first LD_LIBRARY_PATH; on Samsung this can make /system/lib64/libsqlite.so
    # bind to the container's incompatible OpenSSL and abort layer loading before
    # Vulkan interception begins. libnativewindow provides the exact AHB symbols
    # LSFG needs without that unrelated framework dependency chain.
    # libdl is used only by Android provenance diagnostics (dladdr) to identify
    # which module owns the function pointers returned to the Vulkan loader.
    target_link_libraries(lsfg-vk PRIVATE nativewindow dl)
    target_include_directories(lsfg-vk PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
endif()

get_target_property(TOML11_INCLUDE_DIRS toml11 INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(lsfg-vk SYSTEM PRIVATE ${TOML11_INCLUDE_DIRS})

# diagnostics
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(lsfg-vk PROPERTIES
        EXPORT_COMPILE_COMMANDS ON)
endif()

if(LSFGVK_EXCESS_DEBUG)
    message(STATUS "LSFGVK_EXCESS_DEBUG is only compatible with clang")
    target_compile_options(lsfg-vk PRIVATE
        -Weverything
        # disable compat c++ flags
        -Wno-pre-c++20-compat-pedantic
        -Wno-pre-c++17-compat-pedantic
        -Wno-c++98-compat-pedantic
        -Wno-c++98-compat
        # disable other flags
        -Wno-missing-designated-field-initializers
        -Wno-shadow
        -Wno-switch-enum
        -Wno-switch-default
        -Wno-padded
        -Wno-exit-time-destructors
        -Wno-global-constructors
        -Wno-cast-function-type-strict
    )

    set_target_properties(lsfg-vk PROPERTIES
        CXX_CLANG_TIDY clang-tidy)
endif()

# install
install(FILES "${CMAKE_BINARY_DIR}/liblsfg-vk.so"
    DESTINATION lib)
install(FILES "${CMAKE_SOURCE_DIR}/VkLayer_LS_frame_generation.json"
    DESTINATION share/vulkan/implicit_layer.d)
