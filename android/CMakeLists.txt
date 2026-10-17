# libbloodframe.so: Bloodframe, Lossless Scaling Frame Generation 3.1 for native Android (Vulkan).
# Based on lsfg-vk 1.0 by PancakeTAS (MIT) via GameNative's Android fork (this repository's history).
# The framegen backend (framegen/), the Lossless.dll shader extraction (src/extract/) and dxbc, pe-parse and
# volk are linked into ONE shared library behind the small C API of bb_lsfg.h (bb_lsfg_*, bb_lsfg.map).
# The fork's Vulkan layer, toml11 (config file) and UI are not built.
#
# Either as a top-level project (cmake -S android) or through the root CMakeLists.txt's option
# (cmake -S . -DBLOODFRAME=ON); build-android.sh does the NDK cross build.
cmake_minimum_required(VERSION 3.18)
project(bloodframe LANGUAGES C CXX)

# The fork's source tree: the repository root.
if(NOT LSFG_SRC)
  get_filename_component(LSFG_SRC "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT EXISTS "${LSFG_SRC}/framegen/CMakeLists.txt")
  message(FATAL_ERROR "LSFG_SRC is not the Bloodframe source tree: ${LSFG_SRC}")
endif()
if(NOT ANDROID)
  message(FATAL_ERROR "Bloodframe is for Android (AHardwareBuffer, sync files): use the NDK toolchain, see build-android.sh")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(BUILD_SHARED_LIBS OFF)
# The same flags the fork's top level gives its subprojects.
add_compile_options(-Wno-deprecated-declarations -Wno-unused-template)
# VK_ANDROID_external_memory_android_hardware_buffer types (framegen includes the plain headers).
add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)

add_subdirectory(${LSFG_SRC}/thirdparty/dxbc dxbc EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/thirdparty/pe-parse/pe-parser-library pe-parse EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/thirdparty/volk volk EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/framegen framegen EXCLUDE_FROM_ALL)

# framegen/v3.1*_src/context.cpp call close() before their <unistd.h>; libc++ of the NDK does not
# bring it in.
target_compile_options(lsfg-vk-framegen PRIVATE -include unistd.h)

add_library(bloodframe SHARED
  bb_lsfg.cpp
  bb_half.cpp
  ${LSFG_SRC}/src/extract/extract.cpp
  ${LSFG_SRC}/src/extract/trans.cpp)
set_target_properties(bloodframe PROPERTIES
  OUTPUT_NAME bloodframe
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)
target_include_directories(bloodframe PRIVATE ${LSFG_SRC}/include)
# nativewindow: AHardwareBuffer (not libandroid.so, which pulls in the framework); log: logcat.
target_link_libraries(bloodframe PRIVATE lsfg-vk-framegen pe-parse dxbc volk nativewindow log)
# Only the C API leaves the library; the static archives' symbols stay inside (volk's global
# vkCreateInstance etc. must not interpose on anything).
target_link_options(bloodframe PRIVATE
  -Wl,--exclude-libs,ALL
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bb_lsfg.map)
