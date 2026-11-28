cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(BeatTheBeatQilinRecomp VERSION 0.17.1 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules anywhere: without this, Ninja builds scan every C++ source
# for them with clang-scan-deps, which zig's Clang (the Linux download's
# compiler, and a Windows PC's without LLVM) doesn't have.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The DOL loader: runtime code too (no std::format) ----
add_library(qilin_dol STATIC recomp/src/dol.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_dol PUBLIC recomp/src)
target_link_libraries(qilin_dol PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/dol_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/lifter.cpp
    recomp/src/hwaccess.cpp
    recomp/src/libraries.cpp
    recomp/src/identify.cpp
    recomp/src/symbols.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_dol PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Guest runtime (header-only) and the floating-point rules lifted code needs ----
# Lifted code must be bit-exact: no multiply-add contraction and no fast-math.
add_library(qilin_runtime INTERFACE)
target_include_directories(qilin_runtime INTERFACE recomp/runtime)
if(MSVC)
    target_compile_options(qilin_runtime INTERFACE /fp:precise)
else()
    target_compile_options(qilin_runtime INTERFACE -ffp-contract=off -fno-fast-math)
endif()

# Lifted code from `qilin lift` (needs your own main.dol):
#   qilin lift main.dol --out gen && cmake -B build -DQILIN_GEN_DIR=$PWD/gen
# qilin_difftest runs it against the original code (tools/difftest), with
# checked memory: an access outside the mapped guest memory stops.
set(QILIN_GEN_DIR "" CACHE PATH "qilin lift output directory")
# The differential test under UBSan: any undefined behaviour in lifted code
# or the runtime stops the harness (and so the test) with the report.
# MSVC has no UBSan; on Windows, tools/difftest/build_ubsan.py builds the
# harness with zig's Clang instead.
option(QILIN_UBSAN "Build lifted code and qilin_difftest with UBSan (GCC or Clang)" OFF)
if(QILIN_UBSAN AND MSVC)
    message(FATAL_ERROR "QILIN_UBSAN needs GCC or Clang; on Windows use tools/difftest/build_ubsan.py")
endif()
if(QILIN_GEN_DIR)
    file(GLOB QILIN_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_library(qilin_gen_checked STATIC ${QILIN_GEN_SOURCES} recomp/runtime/dispatch.cpp recomp/runtime/timebase.cpp)
    target_include_directories(qilin_gen_checked PUBLIC "${QILIN_GEN_DIR}")
    target_compile_definitions(qilin_gen_checked PUBLIC QILIN_CHECKED_MEMORY QILIN_DETERMINISTIC_TIME)
    target_link_libraries(qilin_gen_checked PUBLIC qilin_runtime PRIVATE qilin_warnings)
    if(MSVC)
        # C4702 (unreachable code) comes from MSVC's optimiser after inlining,
        # e.g. after a call to a lifted function that always stops: it follows
        # inlining decisions, not anything the lifter can see or avoid.
        target_compile_options(qilin_gen_checked PRIVATE /wd4702)
    endif()
    add_executable(qilin_difftest tools/difftest/harness.cpp)
    target_link_libraries(qilin_difftest PRIVATE qilin_gen_checked qilin_dol qilin_warnings)
    qilin_tool(qilin_difftest)
    if(QILIN_UBSAN)
        foreach(t qilin_gen_checked qilin_difftest qilin_dol)
            target_compile_options(${t} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
        endforeach()
        target_link_options(qilin_difftest PRIVATE -fsanitize=undefined)
    endif()
endif()

enable_testing()

# The downloads' launcher (tools/player-kit/launcher, the same files in every
# QilinRecomp project, copied unchanged from Toy Commander's: ADOPTING.md
# there): its core and tests here, its program in the host section below
# (where SDL3, Dear ImGui and volk are known). Its build test runs the real
# build with stand-in children, and qilin's identify.
option(QILIN_REQUIRE_LAUNCHER_TESTS "Require the launcher's build test (CI): a native build, not a cross-compile" OFF)
if(QILIN_REQUIRE_LAUNCHER_TESTS AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "the launcher's build test needs a native build")
endif()
set(QILIN_LAUNCHER_QILIN qilin)
include(tools/player-kit/launcher/launcher.cmake)

# The game's runtime: qilin_run's own sources (the native functions register
# from static initialisers, so they are the executable's sources, or an
# archive linked whole: a plain static library would drop them), and the
# host side (a window, Vulkan, audio and the keyboard) unless headless.
set(QILIN_RUN_SOURCES
    recomp/runtime/run.cpp
    recomp/runtime/machine.cpp
    recomp/runtime/native.cpp
    recomp/runtime/os.cpp
    recomp/runtime/threads.cpp
    recomp/runtime/interrupts.cpp
    recomp/runtime/dvd.cpp
    recomp/runtime/gx.cpp
    recomp/runtime/gx_fifo.cpp
    recomp/runtime/vi.cpp
    recomp/runtime/wpad.cpp
    recomp/runtime/input.cpp
    recomp/runtime/audio.cpp
    recomp/runtime/nand.cpp
    recomp/runtime/profile.cpp
    recomp/runtime/coherence.cpp
    recomp/runtime/settings.cpp
    recomp/runtime/bot.cpp
)
set(QILIN_RUN_HOST_SOURCES recomp/runtime/host.cpp recomp/runtime/gx_vulkan.cpp recomp/runtime/gx_texture.cpp
                           recomp/runtime/menu.cpp recomp/runtime/pause.cpp)
if(WIN32)
    list(APPEND QILIN_RUN_HOST_SOURCES recomp/runtime/wasapi.cpp)  # exclusive-mode audio
endif()
# Compiled with the lifted code in a developer's build (qilin_gen_runtime),
# into the player kit's runtime otherwise.
set(QILIN_RUN_GEN_SIDE_SOURCES recomp/runtime/dispatch.cpp recomp/runtime/timebase.cpp)
# winmm: timeBeginPeriod (1 ms timers for the audio clock); avrt: the
# multimedia scheduler's classes for the game's and audio's threads; dwmapi:
# the compositor's vblank times (the vblank lock); ole32: COM for WASAPI's
# exclusive mode; dbghelp: the sampling profiler's names.
set(QILIN_RUN_WIN32_LIBRARIES winmm avrt dwmapi ole32 dbghelp)
set_source_files_properties(recomp/runtime/menu.cpp PROPERTIES COMPILE_DEFINITIONS QILIN_VERSION="${PROJECT_VERSION}")  # the menu's About

# Lifted code from `qilin lift --runtime` (a second directory: the
# difftest's lifts every library, this one calls native code for the
# replaced ones), for a developer's qilin_run:
#   qilin symbols main.dol --decomp third_party/rhf/SOME01 -o pal_symbols.txt
#   qilin lift main.dol --out gen-rt --runtime --symbols pal_symbols.txt
#   cmake -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt
set(QILIN_RUNTIME_GEN_DIR "" CACHE PATH "qilin lift --runtime output directory")
option(QILIN_HEADLESS_ONLY "Build qilin_run without a window, Vulkan or audio" OFF)
# The players' downloads (tools/player-kit): the runtime without the lifted
# code, as archives the launcher links the player's own lifted code with.
option(QILIN_PLAYER_KIT "Build the player kit (tools/player-kit): the game's runtime as archives, for the downloads" OFF)
# The launcher's program on its own (it comes with the player kit).
option(QILIN_LAUNCHER "Build the players' launcher program (fetches SDL3, Dear ImGui and volk)" OFF)
if(QILIN_PLAYER_KIT AND QILIN_RUNTIME_GEN_DIR)
    message(FATAL_ERROR "the player kit is built without lifted code: leave QILIN_RUNTIME_GEN_DIR empty")
endif()
if(QILIN_PLAYER_KIT AND QILIN_HEADLESS_ONLY)
    message(FATAL_ERROR "the player kit is the windowed game: leave QILIN_HEADLESS_ONLY off")
endif()

# ---- The host side's libraries ----
# Third-party code is fetched at configure time, pinned by hash, and never
# committed: SDL3 (zlib licence; a system SDL3 is used if there is one, but
# never for the player kit, which needs SDL built statically), Khronos'
# Vulkan headers (Apache-2.0; the loader comes through SDL, so no Vulkan SDK
# is needed), Dear ImGui (MIT) and, for the launcher only, volk (MIT). The
# shaders' SPIR-V is committed beside their GLSL (recomp/runtime/shaders/spirv,
# our own code compiled); a stamp of the GLSL's hashes stops the configure if
# they drift apart, and QILIN_BUILD_SHADERS=ON fetches glslang
# (BSD-3/Apache-2.0) to regenerate them. Only fetched when something needs
# them: the game's build (not QILIN_HEADLESS_ONLY), the player kit, the launcher.
if((QILIN_RUNTIME_GEN_DIR AND NOT QILIN_HEADLESS_ONLY) OR QILIN_PLAYER_KIT OR QILIN_LAUNCHER)
    include(FetchContent)
    if(NOT QILIN_PLAYER_KIT)
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(NOT SDL3_FOUND)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)    # the kit installs only its own files
        set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.20.tar.gz
            URL_HASH SHA256=5cb56d4223a41283f3c2baf6f1263fba171692409ae405a50417a7c8e7e28deb)
        FetchContent_MakeAvailable(SDL3)
        # SDL's Wayland protocol headers, made from its own XML, before the
        # system's (as Toy Commander's): SDL adds them as a system folder, and
        # zig's compiler (the Linux download's) searches the system's
        # /usr/include first, whose older wayland-client-protocol.h (Ubuntu
        # 22.04's) lacks what SDL uses. SDL includes them in quotes, and
        # -iquote folders come before every -I and -isystem one.
        if(TARGET SDL3-static AND EXISTS "${sdl3_BINARY_DIR}/wayland-generated-protocols" AND NOT MSVC)
            target_compile_options(SDL3-static BEFORE PRIVATE "-iquote${sdl3_BINARY_DIR}/wayland-generated-protocols")
        endif()
    endif()
    FetchContent_Declare(VulkanHeaders
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.299.tar.gz
        URL_HASH SHA256=95bb8fe731469b9e1be532b9d3b4d7d33e28ddbd99a926da7f0eca82a134b92f)
    FetchContent_MakeAvailable(VulkanHeaders)
    # The shaders: GLSL in recomp/runtime/shaders, SPIR-V headers
    # (uint32_t arrays) in its spirv folder, and the GLSL's SHA-256
    # (line endings normalised) in spirv/sources.sha256.
    set(QILIN_SHADERS gx.vert gx.frag copy.vert copy.frag present.vert crt.frag)
    set(QILIN_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime/shaders")
    set(QILIN_SHADER_DIR "${QILIN_SHADER_SRC}/spirv")
    set(stamp "")
    foreach(shader ${QILIN_SHADERS})
        file(READ "${QILIN_SHADER_SRC}/${shader}" text)
        string(REPLACE "\r" "" text "${text}")
        string(SHA256 hash "${text}")
        string(APPEND stamp "${hash}  ${shader}\n")
        # an edited shader re-runs the configure, so the check (or the stamp) follows it
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${QILIN_SHADER_SRC}/${shader}")
    endforeach()
    option(QILIN_BUILD_SHADERS "Fetch glslang and regenerate the committed SPIR-V from the GLSL" OFF)
    if(QILIN_BUILD_SHADERS)
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
        set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glslang
            URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
            URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
        FetchContent_MakeAvailable(glslang)
        set(QILIN_SHADER_HEADERS "")
        foreach(shader ${QILIN_SHADERS})
            string(REPLACE "." "_" name "${shader}")
            set(out "${QILIN_SHADER_DIR}/${name}.h")
            add_custom_command(OUTPUT "${out}"
                COMMAND glslang-standalone -V --target-env vulkan1.3 --vn "k_${name}" -o "${out}" "${QILIN_SHADER_SRC}/${shader}"
                DEPENDS "${QILIN_SHADER_SRC}/${shader}" glslang-standalone
                VERBATIM)
            list(APPEND QILIN_SHADER_HEADERS "${out}")
        endforeach()
        file(WRITE "${QILIN_SHADER_DIR}/sources.sha256" "${stamp}")
        add_custom_target(qilin_shaders DEPENDS ${QILIN_SHADER_HEADERS})
    else()
        file(READ "${QILIN_SHADER_DIR}/sources.sha256" committed)
        string(REPLACE "\r" "" committed "${committed}")
        if(NOT committed STREQUAL stamp)
            message(FATAL_ERROR "The shaders' GLSL changed since their SPIR-V was generated: "
                                "configure once with -DQILIN_BUILD_SHADERS=ON and commit recomp/runtime/shaders/spirv")
        endif()
    endif()

    # Dear ImGui (MIT) for the settings menu, as the other Qilin recomps
    # draw theirs: the same release (v1.93.0), its core and its SDL3 and
    # Vulkan backends, fetched and pinned like SDL3. Its Vulkan backend
    # loads the functions through the renderer's loader (no prototypes).
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114)
    FetchContent_MakeAvailable(imgui)
    add_library(qilin_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC SDL3::SDL3 Vulkan::Headers)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()

    # volk (MIT), for the launcher's Vulkan check only (the game loads Vulkan
    # through SDL): one C file, fetched and pinned, without its own CMake
    # project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk
        URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
        URL_HASH SHA256=8ffd0e81e29688f4abaa39e598937160b098228f37503903b10d481d4862ab85
        SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(volk)
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    # The downloads' launcher, `Play Beat the Beat` (Windows: .exe; macOS:
    # the app's program), which reads kit/game.txt (tools/player-kit/launcher).
    find_package(Threads REQUIRED)
    qilin_launcher_program(IMGUI_DIR ${imgui_SOURCE_DIR} LIBRARIES qilin_imgui qilin_volk SDL3::SDL3 Threads::Threads)
    if(WIN32)
        # The launcher's icon for the game too, which the player's PC links
        # (the player kit's icon.o; QILIN_WINDOWS_RESOURCES is launcher.cmake's).
        if(QILIN_WINDOWS_RESOURCES)
            add_library(qilin_game_icon OBJECT tools/player-kit/game_icon.rc)
        elseif(QILIN_PLAYER_KIT)
            message(FATAL_ERROR "the player kit needs QILIN_WINDOWS_RESOURCES=ON (the game's icon)")
        endif()
    endif()
endif()

# The game: qilin_run, from `qilin lift --runtime` output.
# Checked memory for now (bring-up): an access outside the memory lifted code
# may use stops with the address and the instruction.
if(QILIN_RUNTIME_GEN_DIR)
    file(GLOB QILIN_RUNTIME_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_RUNTIME_GEN_DIR}/*.cpp")
    add_library(qilin_gen_runtime STATIC ${QILIN_RUNTIME_GEN_SOURCES} ${QILIN_RUN_GEN_SIDE_SOURCES})
    target_include_directories(qilin_gen_runtime PUBLIC "${QILIN_RUNTIME_GEN_DIR}")
    # QILIN_APPROX_ESTIMATES: frsqrte/fres approximated where Broadway's table
    # is needed (guest.hpp); the difftest's build keeps stopping there.
    target_compile_definitions(qilin_gen_runtime PUBLIC QILIN_APPROX_ESTIMATES)
    # Checked memory: every lifted load and store tested against the regions
    # lifted code may use, stopping with the address and the instruction.
    # For bring-up (the default); OFF for speed once a run is known good.
    option(QILIN_RUNTIME_CHECKED "Check every guest memory access of lifted code (bring-up)" ON)
    if(QILIN_RUNTIME_CHECKED)
        target_compile_definitions(qilin_gen_runtime PUBLIC QILIN_CHECKED_MEMORY)
    endif()
    target_link_libraries(qilin_gen_runtime PUBLIC qilin_runtime PRIVATE qilin_warnings)
    if(MSVC)
        target_compile_options(qilin_gen_runtime PRIVATE /wd4702)  # as for qilin_gen_checked
    endif()
    add_executable(qilin_run ${QILIN_RUN_SOURCES})
    target_link_libraries(qilin_run PRIVATE qilin_gen_runtime qilin_dol qilin_warnings)
    if(WIN32)
        target_link_libraries(qilin_run PRIVATE ${QILIN_RUN_WIN32_LIBRARIES})
    endif()
    # Debug symbols in an optimised build, for the sampling profiler
    # (QILIN_SAMPLE=1) and debuggers: the same code, with names.
    option(QILIN_DEBUG_SYMBOLS "Debug symbols in the game's optimised build (profiling)" OFF)
    if(QILIN_DEBUG_SYMBOLS)
        if(MSVC)
            target_compile_options(qilin_gen_runtime PRIVATE /Zi)
            target_compile_options(qilin_run PRIVATE /Zi)
            target_link_options(qilin_run PRIVATE /DEBUG /OPT:REF /OPT:ICF)
        else()
            target_compile_options(qilin_gen_runtime PRIVATE -g)
            target_compile_options(qilin_run PRIVATE -g)
        endif()
    endif()
    # Link-time optimisation: lifted functions call each other across the
    # generated files; LTO lets the compiler inline across them. Slower to
    # build.
    option(QILIN_LTO "Link-time optimisation for the game's code" OFF)
    if(QILIN_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT qilin_ipo OUTPUT qilin_ipo_why)
        if(NOT qilin_ipo)
            message(FATAL_ERROR "QILIN_LTO: this toolchain can't do link-time optimisation: ${qilin_ipo_why}")
        endif()
        set_property(TARGET qilin_gen_runtime qilin_run PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
    # QILIN_HEADLESS_ONLY builds without a window, Vulkan or audio
    # (QILIN_HEADLESS=1 also runs a full build without a window).
    if(QILIN_HEADLESS_ONLY)
        target_compile_definitions(qilin_run PRIVATE QILIN_HEADLESS_ONLY)
    else()
        if(QILIN_BUILD_SHADERS)
            add_dependencies(qilin_run qilin_shaders)
        endif()
        target_sources(qilin_run PRIVATE ${QILIN_RUN_HOST_SOURCES})
        target_include_directories(qilin_run PRIVATE "${QILIN_SHADER_DIR}")
        target_link_libraries(qilin_run PRIVATE SDL3::SDL3 Vulkan::Headers qilin_imgui)
    endif()
endif()

# ---- The player kit: everything the game needs but the code lifted from main.dol ----
# For the downloads (tools/player-kit), as Toy Commander's and Punch-Out's:
#   kit/lib/libbeatthebeat_kit.a   the runtime (qilin_run's sources, the
#                                  dispatcher and time base), linked whole:
#                                  its native functions register themselves
#   kit/lib/libbeatthebeat_libs.a  what it links: the DOL loader, Dear ImGui, SDL3
#   kit/include/                   the headers the lifted code includes
#   kit/qilin, kit/rhf/            the recompiler, and the decomp's symbols
#                                  and splits that `qilin symbols` ports
#   kit/build-flags.txt            how the launcher (tools/player-kit/launcher,
#                                  build.cpp) compiles and links the code it
#                                  lifts from the player's own main.dol
# Windows: cross-compiled on Linux with llvm-mingw (package.sh). Linux: built
# with the zig the download bundles, so the archives' libc++ is the one the
# player's compiles link with (package-linux.sh). macOS: universal, with
# Apple's clang (package-macos.sh, tools/player-kit/macos.cmake). The
# memory checks are off, as the installer's default build.
# What the lifted code is compiled with, in build-flags.txt: as
# qilin_gen_runtime's (CMake's Release flags, qilin_runtime's floating-point
# rules, QILIN_APPROX_ESTIMATES).
set(QILIN_LIFTED_OPTIONS -ffp-contract=off -fno-fast-math -DQILIN_APPROX_ESTIMATES)
if(APPLE)
    include(tools/player-kit/macos.cmake)
endif()
# The runtime without lifted code, wherever the host side's libraries are
# there and no lifted code is: the player kit's, and (QILIN_LAUNCHER) a
# check that the whole game program compiles and links with stand-ins for
# the lifted code (beatthebeat_runtime_check, tools/player-kit/link_check.cpp).
if(TARGET qilin_imgui AND NOT QILIN_RUNTIME_GEN_DIR)
    add_library(beatthebeat_runtime STATIC ${QILIN_RUN_SOURCES} ${QILIN_RUN_HOST_SOURCES} ${QILIN_RUN_GEN_SIDE_SOURCES})
    target_compile_definitions(beatthebeat_runtime PRIVATE QILIN_APPROX_ESTIMATES)
    target_include_directories(beatthebeat_runtime PRIVATE "${QILIN_SHADER_DIR}")
    target_link_libraries(beatthebeat_runtime PRIVATE qilin_runtime qilin_dol SDL3::SDL3 Vulkan::Headers qilin_imgui qilin_warnings)
    if(WIN32)
        target_link_libraries(beatthebeat_runtime PRIVATE ${QILIN_RUN_WIN32_LIBRARIES})
    endif()
    if(QILIN_BUILD_SHADERS)
        add_dependencies(beatthebeat_runtime qilin_shaders)
    endif()
    if(NOT QILIN_PLAYER_KIT)
        add_executable(beatthebeat_runtime_check tools/player-kit/link_check.cpp)
        target_include_directories(beatthebeat_runtime_check PRIVATE recomp/runtime)
        target_link_libraries(beatthebeat_runtime_check PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,beatthebeat_runtime>" qilin_warnings)
    endif()
endif()
if(QILIN_PLAYER_KIT)
    if(NOT TARGET SDL3-static)
        message(FATAL_ERROR "the player kit needs SDL3 built statically (it is fetched: no system SDL3)")
    endif()
    set(kit_whole beatthebeat_runtime)
    set(kit_libraries qilin_dol qilin_imgui SDL3-static)
    set(kit_runtime "${CMAKE_BINARY_DIR}/kit/libbeatthebeat_kit.a")
    set(kit_archive "${CMAKE_BINARY_DIR}/kit/libbeatthebeat_libs.a")
    string(JOIN " " lifted_text ${QILIN_LIFTED_OPTIONS})
    if(APPLE)
        qilin_player_kit_macos()  # tools/player-kit/macos.cmake
    else()
        set(mri "CREATE ${kit_archive}\n")
        foreach(library IN LISTS kit_libraries)
            string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
        endforeach()
        string(APPEND mri "SAVE\nEND\n")
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/kit/beatthebeat_libs.mri" CONTENT "${mri}")
        add_custom_command(OUTPUT "${kit_archive}" "${kit_runtime}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/kit"
            COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/kit/beatthebeat_libs.mri" "-DOUT=${kit_archive}"
                    -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${kit_whole}>" "${kit_runtime}"
            DEPENDS ${kit_libraries} ${kit_whole} "${CMAKE_BINARY_DIR}/kit/beatthebeat_libs.mri"
            COMMENT "Merging the player kit's libraries" VERBATIM)
        add_custom_target(beatthebeat_kit ALL DEPENDS "${kit_archive}" "${kit_runtime}")
        if(WIN32)
            # The system libraries SDL3 and the runtime need (MinGW names).
            # The game's threads take the program's stack size (8 MB, as on
            # Linux): lifted calls nest as deeply as the guest's.
            set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
                -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32 -lavrt -ldwmapi -ldbghelp)
            set(kit_ldflags "-static -O2 -Wl,--stack,8388608")
            set(kit_icon " $kit/lib/icon.o")
            set(kit_native "")  # the launcher's default, -march=native
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # SDL3 loads X11, Wayland, the sound servers and Vulkan at run
            # time: the game links only glibc's own libraries (and zig's libc++).
            set(kit_system_libraries -lm -ldl -lpthread -lrt)
            set(kit_ldflags "-O2")
            set(kit_icon "")
            # For the player's processor: x86-64's -march, ARM's -mcpu.
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
                set(kit_native "native = -mcpu=native\n")
            else()
                set(kit_native "native = -march=native\n")
            endif()
        else()
            message(FATAL_ERROR "no player kit for ${CMAKE_SYSTEM_NAME} yet")
        endif()
        string(JOIN " " kit_system_text ${kit_system_libraries})
        # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value`
        # lines, $kit the kit's folder. The runtime's archive is linked whole.
        set(kit_whole_flags "-Wl,--whole-archive $kit/lib/libbeatthebeat_kit.a -Wl,--no-whole-archive")
        file(WRITE "${CMAKE_BINARY_DIR}/kit/build-flags.txt"
            "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
            "cxxflags = -O2 -DNDEBUG -std=c++23 -w ${lifted_text} -I$kit/include\n"
            "${kit_native}"
            "ldflags = ${kit_ldflags}\n"
            "libs = ${kit_whole_flags} $kit/lib/libbeatthebeat_libs.a${kit_icon} ${kit_system_text}\n")
        # Linked with stand-ins for the lifted code: the kit is complete.
        add_executable(beatthebeat_kit_link_check tools/player-kit/link_check.cpp)
        target_include_directories(beatthebeat_kit_link_check PRIVATE recomp/runtime)
        target_link_libraries(beatthebeat_kit_link_check PRIVATE -Wl,--whole-archive "${kit_runtime}" -Wl,--no-whole-archive
                              "${kit_archive}" ${kit_system_libraries})
        add_dependencies(beatthebeat_kit_link_check beatthebeat_kit)
        if(WIN32)
            target_link_options(beatthebeat_kit_link_check PRIVATE -static)
            # The game's icon (the launcher's), linked into it on the player's PC.
            add_dependencies(beatthebeat_kit qilin_game_icon)
            install(FILES $<TARGET_OBJECTS:qilin_game_icon> DESTINATION kit/lib RENAME icon.o COMPONENT beatthebeat)
        endif()
        install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT beatthebeat)
        install(FILES "${kit_runtime}" "${kit_archive}" DESTINATION kit/lib COMPONENT beatthebeat)
        install(FILES "${CMAKE_BINARY_DIR}/kit/build-flags.txt" DESTINATION kit COMPONENT beatthebeat)
    endif()
    # What the lifted code includes, and what `qilin symbols` reads (the
    # decomp's files, CC0: the player's computer ports the names to their own
    # main.dol, game.txt's step.1).
    install(FILES recomp/runtime/guest.hpp recomp/runtime/native.hpp DESTINATION kit/include COMPONENT beatthebeat)
    install(FILES third_party/rhf/SOME01/symbols.txt third_party/rhf/SOME01/splits.txt DESTINATION kit/rhf COMPONENT beatthebeat)
endif()

# ---- Tests (no game files needed) ----
add_executable(qilin_nand_io_test recomp/tests/nand_io_test.cpp)
target_include_directories(qilin_nand_io_test PRIVATE recomp/runtime)
target_link_libraries(qilin_nand_io_test PRIVATE qilin_warnings)
add_test(NAME qilin_nand_io_test COMMAND qilin_nand_io_test)
add_executable(qilin_tests recomp/tests/tests.cpp recomp/runtime/native.cpp recomp/runtime/interrupts.cpp recomp/runtime/timebase.cpp
    recomp/runtime/gx_texture.cpp recomp/runtime/input.cpp recomp/runtime/profile.cpp recomp/runtime/settings.cpp recomp/runtime/bot.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_runtime qilin_warnings)
target_compile_definitions(qilin_tests PRIVATE QILIN_HEADLESS_ONLY)  # input.cpp without the host side
if(WIN32)
    target_link_libraries(qilin_tests PRIVATE dbghelp)  # profile.cpp's sampler names
endif()
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)

