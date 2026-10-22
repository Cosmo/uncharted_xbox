cmake_minimum_required(VERSION 3.20)
project(uncharted_uwp LANGUAGES C CXX)
option(UNCHARTED_BUILD_GRAPHICS "Build the existing renderer for UWP" OFF)
option(UNCHARTED_LINK_RENDERER "Connect the existing renderer to the UWP loader" ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang-cl: guest code uses the System V x64 ABI")
endif()
get_filename_component(UNCHARTED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# No path of the build machine in the package: __FILE__ (asserts, logs) relative to the checkout.
add_compile_options("/clang:-ffile-prefix-map=${UNCHARTED_ROOT}/=")
set(UNCHARTED_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.local/deps" CACHE PATH "Pinned third-party dependencies (scripts/build-deps.ps1)")
set(UNCHARTED_PTHREAD "${UNCHARTED_DEPS_ROOT}/winpthreads" CACHE PATH "UCRT winpthread headers and import library")
# platform/compat/unistd.h owns mode_t; winpthreads 14 would declare its own, narrower one.
add_compile_definitions(_MODE_T_)
# The app prepares the game files from the player's dump at run time; only their format is built in.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt" UNCHARTED_PREPARED_FORMAT LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/prepared-format.txt")
set(UNCHARTED_UWP_SOURCES
  runtime runtime_content runtime_fiber runtime_file runtime_kernel runtime_memory runtime_mutex
  runtime_rtc runtime_rwlock runtime_savedata runtime_sema runtime_services runtime_thread runtime_time
  compat_win probe)
list(TRANSFORM UNCHARTED_UWP_SOURCES PREPEND "${UNCHARTED_ROOT}/src/")
list(TRANSFORM UNCHARTED_UWP_SOURCES APPEND ".c")
add_executable(uncharted_uwp WIN32 platform/main.cpp platform/prepare_game.cpp platform/runtime_compat.c platform/milestone_services.c ${UNCHARTED_UWP_SOURCES})
target_include_directories(uncharted_uwp PRIVATE "${UNCHARTED_ROOT}" "${UNCHARTED_ROOT}/src" platform platform/compat "${UNCHARTED_PTHREAD}/include")
target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_UWP NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _TIMESPEC_DEFINED
  UNCHARTED_PREPARED_FORMAT="${UNCHARTED_PREPARED_FORMAT}")
target_compile_options(uncharted_uwp PRIVATE /W3 $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-deprecated-declarations> $<$<COMPILE_LANGUAGE:C>:/clang:-masm=att> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
# /PDBALTPATH: the executable names its symbols file without the build folder.
target_link_options(uncharted_uwp PRIVATE /APPCONTAINER /HIGHENTROPYVA:NO /STACK:16777216,16777216 /OPT:REF /PDBALTPATH:%_PDB%)
target_link_directories(uncharted_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
target_link_libraries(uncharted_uwp PRIVATE WindowsApp "${UNCHARTED_PTHREAD}/lib/libwinpthread.dll.a")
set(UNCHARTED_LAYOUT "${CMAKE_BINARY_DIR}/layout")
if(UNCHARTED_BUILD_GRAPHICS)
  add_compile_options(/clang:-Wno-unused-command-line-argument)
  set(UNCHARTED_UWP ON)
  set(BB_LTO OFF CACHE BOOL "" FORCE)
  add_subdirectory("${UNCHARTED_ROOT}/gpu" "${CMAKE_BINARY_DIR}/gpu")
  add_executable(uwp-dialog-input-test EXCLUDE_FROM_ALL ../tests/test_uwp_dialog_input.cpp graphics/window.cpp)
  target_include_directories(uwp-dialog-input-test PRIVATE "${UNCHARTED_ROOT}/gpu/shim" "${UNCHARTED_ROOT}/gpu/shadps4")
  target_compile_definitions(uwp-dialog-input-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(uwp-dialog-input-test PRIVATE Vulkan::Headers PkgConfig::SDL3)
  target_sources(bbgpu PRIVATE graphics/overlay.cpp graphics/texture_probe.cpp)
  target_sources(uncharted_uwp PRIVATE graphics/probe.cpp graphics/present_bridge.cpp graphics/descriptor_fallback.cpp)
  target_include_directories(uncharted_uwp PRIVATE "${UNCHARTED_ROOT}/gpu/shadps4")
  target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_BUILD_GRAPHICS VK_USE_PLATFORM_WIN32_KHR)
  target_link_libraries(uncharted_uwp PRIVATE Vulkan::Headers d3d12 dxgi)
  if(UNCHARTED_LINK_RENDERER)
    target_compile_definitions(uncharted_uwp PRIVATE UNCHARTED_LINK_RENDERER)
    target_sources(uncharted_uwp PRIVATE platform/input.cpp platform/audio.cpp "${UNCHARTED_ROOT}/src/runtime_pad.c"
      "${UNCHARTED_ROOT}/src/runtime_audio.c" "${UNCHARTED_ROOT}/src/runtime_ajm.c"
      platform/desktop_shims.cpp)
    file(GLOB BB_ATRAC9 "${UNCHARTED_ROOT}/third_party/LibAtrac9/C/src/*.c")
    add_library(uncharted_atrac9 STATIC ${BB_ATRAC9})
    set_source_files_properties("${UNCHARTED_ROOT}/src/runtime_ajm.c" PROPERTIES COMPILE_DEFINITIONS COMPILING_DLL)
    target_link_libraries(uncharted_uwp PRIVATE bbgpu uncharted_atrac9 PkgConfig::SDL3 PkgConfig::FFMPEG xaudio2)
    target_compile_definitions(bbgpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
  endif()
  # Dozen comes from the Mesa fork's UWP build (a local checkout, or its release on GitHub), dxil.dll from DXC.
  set(UNCHARTED_DOZEN_DLL "${UNCHARTED_MESA_ROOT}/build/dozen-uwp/src/microsoft/vulkan/vulkan_dzn.dll" CACHE FILEPATH "Dozen built for UWP")
  set(UNCHARTED_MESA_LICENSE "${UNCHARTED_MESA_ROOT}/mesa/licenses/MIT" CACHE FILEPATH "Mesa's MIT license")
  set(UNCHARTED_DXC_ROOT "${UNCHARTED_MESA_ROOT}/tools/dxc" CACHE PATH "DirectX Shader Compiler release (bin/x64/dxil.dll and licenses)")
  # The license of every library in the package (CREDITS.md lists them).
  set(UNCHARTED_LICENSE_COMMANDS)
  foreach(entry IN ITEMS
      "${UNCHARTED_MESA_LICENSE}|Mesa-MIT.txt"
      "${UNCHARTED_DXC_ROOT}/LICENCE-MIT.txt|DXC-LICENCE-MIT.txt" "${UNCHARTED_DXC_ROOT}/LICENSE-LLVM.txt|DXC-LICENSE-LLVM.txt"
      "${UNCHARTED_DXC_ROOT}/LICENSE-MS.txt|DXC-LICENSE-MS.txt"
      "${UNCHARTED_ROOT}/gpu/shadps4/LICENSE|shadPS4-GPL-2.0.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/ffmpeg.txt|FFmpeg-LGPL-2.1.txt" "${UNCHARTED_DEPS_ROOT}/licenses/sdl.txt|SDL3-zlib.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/fmt.txt|fmt-MIT.txt" "${UNCHARTED_DEPS_ROOT}/licenses/zydis.txt|Zydis-MIT.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/zycore.txt|Zycore-MIT.txt" "${UNCHARTED_DEPS_ROOT}/licenses/xxhash.txt|xxHash-BSD-2.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/vma.txt|VulkanMemoryAllocator-MIT.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/vulkan-headers.txt|Vulkan-Headers.txt"
      "${UNCHARTED_DEPS_ROOT}/licenses/robin-map.txt|robin-map-MIT.txt" "${UNCHARTED_DEPS_ROOT}/boost/licenses/config.txt|Boost-BSL-1.0.txt"
      "${UNCHARTED_ROOT}/third_party/LibAtrac9/LICENSE|LibAtrac9-MIT.txt" "${UNCHARTED_ROOT}/gpu/third_party/imgui/LICENSE.txt|imgui-MIT.txt"
      "${UNCHARTED_ROOT}/gpu/third_party/miniz/LICENSE|miniz-MIT.txt" "${UNCHARTED_ROOT}/gpu/third_party/magic_enum/LICENSE|magic_enum-MIT.txt"
      "${UNCHARTED_ROOT}/gpu/third_party/half/LICENSE.txt|half-MIT.txt" "${UNCHARTED_ROOT}/gpu/third_party/sirit/LICENSE.txt|sirit-BSD-3.txt"
      "${UNCHARTED_ROOT}/gpu/third_party/xbyak/COPYRIGHT|xbyak-BSD-3.txt"
      "${UNCHARTED_ROOT}/gpu/third_party/fsr-vulkan/LICENSE.txt|fsr-vulkan-MIT.txt"
      "${UNCHARTED_ROOT}/gpu/third_party/fonts/LICENSE-DejaVu.txt|DejaVu-fonts.txt")
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 source)
    list(GET pair 1 name)
    list(APPEND UNCHARTED_LICENSE_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different "${source}" "${UNCHARTED_LAYOUT}/ThirdParty/${name}")
  endforeach()
  add_custom_command(TARGET uncharted_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UNCHARTED_LAYOUT}/ThirdParty"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DOZEN_DLL}" "${UNCHARTED_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DXC_ROOT}/bin/x64/dxil.dll" "${UNCHARTED_LAYOUT}"
    ${UNCHARTED_LICENSE_COMMANDS})
endif()
add_custom_command(TARGET uncharted_uwp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${UNCHARTED_LAYOUT}/Assets" "${UNCHARTED_LAYOUT}/ThirdParty"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:uncharted_uwp>" "${UNCHARTED_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_DEPS_ROOT}/libwinpthread/bin/libwinpthread-1.dll" "${UNCHARTED_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${UNCHARTED_PTHREAD}/share/licenses/winpthreads/COPYING" "${UNCHARTED_LAYOUT}/ThirdParty/winpthread-COPYING.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UNCHARTED_ROOT}/LICENSE" "${UNCHARTED_LAYOUT}/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md" "${UNCHARTED_LAYOUT}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/AppxManifest.xml" "${UNCHARTED_LAYOUT}")
