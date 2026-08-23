# thps_p8 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in ../config/generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(thps_p8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(../config/generated/rexglue.cmake)

# Sources
set(THPSP8_SOURCES
    src/main.cpp
)

# Windows only. Supplies the C23 math functions the UCRT lacks but that clang
# emits calls to from SIMDe's builtins - see the file for the exact chain. Inert
# everywhere else; the whole translation unit is behind #if defined(_WIN32).
if(WIN32)
    list(APPEND THPSP8_SOURCES src/win_math_compat.cpp)
endif()

# Header-only app modules. Ninja discovers these through the compiler depfile
# either way; listing them is so they show up in IDE/tooling project views
# alongside the one translation unit that pulls them all in.
set(THPSP8_HEADERS
    src/thps_p8_app.h
    src/console_input_guard.h
    src/debug_panel.h
    src/dev_cheats.h
    src/dev_console.h
    src/guest_call.h
    src/guest_probe.h
    src/dump_gate.h
    src/guest_ring_wait.h
    src/guest_swap_wait.h
    src/guest_u8x4_unpack.h
    src/guest_vertex_unpack.h
    ../common/supported_dumps.h
    src/native_indicator.h
    src/parity_capture.h
    src/rexglue_script_input.h
    src/rexglue_input_record.h
)

if(WIN32)
    add_executable(thps_p8 WIN32 ${THPSP8_SOURCES} ${THPSP8_HEADERS})
else()
    add_executable(thps_p8 ${THPSP8_SOURCES} ${THPSP8_HEADERS})
endif()

rexglue_setup_target(thps_p8)

# Keep the machine that built this out of the binary.
#
# __FILE__ expands to whatever absolute path the compiler was handed, so asserts
# and log macros bake the builder's home directory and directory layout into
# every shipped executable. Stripping debug info does not remove them: they are
# string literals in .rodata, not debug records.
#
# clang-cl ignores the GNU spelling silently - it warns and carries on - so the
# Windows build needs the /clang: passthrough or it quietly keeps leaking.
function(thps_p8_anonymise_paths target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "/clang:-ffile-prefix-map=$ENV{HOME}=."
            "/clang:-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/..=.")
    else()
        target_compile_options(${target} PRIVATE
            "-ffile-prefix-map=$ENV{HOME}=."
            "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/..=.")
    endif()
endfunction()

thps_p8_anonymise_paths(thps_p8)

# `..` so the shared dump table is reached as "common/supported_dumps.h" - the
# same spelling thps_p8_identify uses, because it is the same file. Two gates,
# one table (dump_gate.h).
target_include_directories(thps_p8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# debug_panel.h includes <imgui.h>. A prebuilt SDK prefix has that header in its
# own include/ and this is a no-op; a from-source SDK (REXSDK_DIR, which is how
# a build gets the runtime patches compiled in) builds imgui as an OBJECT
# library whose include directory does not reach us on its own.
if(TARGET imgui)
    get_target_property(_thps_imgui_inc imgui INTERFACE_INCLUDE_DIRECTORIES)
    if(_thps_imgui_inc)
        target_include_directories(thps_p8 PRIVATE ${_thps_imgui_inc})
    elseif(REXSDK_DIR)
        target_include_directories(thps_p8 PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
    endif()
endif()

# The launcher. Runs the game as a child and reclaims what its teardown could
# not - see src/launcher/platform.h for why that cannot live in the game.
# Links nothing from the SDK on purpose: it must still build and run when the
# game does not, which is when it is most needed. A console app on Windows too
# (no WIN32 flag) - it is a CLI wrapper and needs its stdio.
add_executable(thps_p8_launch
    src/launcher/main.cpp
    src/launcher/platform.h
    src/launcher/platform_linux.inc
    src/launcher/platform_windows.inc
    src/launcher/platform_apple.inc
    src/launcher/platform_generic.inc
)
target_compile_features(thps_p8_launch PRIVATE cxx_std_23)
thps_p8_anonymise_paths(thps_p8_launch)
