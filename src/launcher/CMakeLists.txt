# thps_p8_gui - the launcher's front end.
#
# A separate CMake project on purpose. Two rules hold it apart from the game:
#
#   1. The rexglue SDK never reaches this project's find_package() calls. The
#      SDK pins SDL3::SDL3 to its own static 3.5.0 the moment the game's
#      CMakeLists is parsed; RmlUi's SDL renderer needs SDL3_image, which only
#      exists against the system's shared build. Mixing them silently aliases
#      one onto the other.
#   2. Nothing here links the SDK at all. Disc identity is answered
#      by `thps_p8 --identify_disc`, so this binary stays a UI over two
#      subprocess calls and cannot fail in the dynamic linker.
#
# Configure standalone, never from the game's build tree:
#   cmake -S . -B out -G Ninja
cmake_minimum_required(VERSION 3.24)
project(thps_p8_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Before anything else defines a target, including the RmlUi that FetchContent
# builds below.
#
# __FILE__ bakes absolute paths into string literals, and stripping does not
# remove them because they live in .rodata rather than in debug records. A
# per-target option would only cover the sources in this file: RmlUi is its own
# target, its headers assert liberally, and it contributed 201 absolute paths to
# the launcher on its own - more than everything else combined.
if(MSVC)
  add_compile_options("/clang:-ffile-prefix-map=$ENV{HOME}=.")
else()
  add_compile_options("-ffile-prefix-map=$ENV{HOME}=.")
endif()

# --- SDL3, shared ------------------------------------------------------------
# NO_CMAKE_PATH/NO_CMAKE_ENVIRONMENT_PATH are belt-and-braces for rule 1 above:
# RmlUi's own DependenciesForBackends.cmake does an unguarded bare
# find_package(SDL3_image), so a stray CMAKE_PREFIX_PATH entry pointing at the
# SDK would be found and would silently alias the SDK's static SDL3 in.
#
# That protection has to be switchable, because it works by ignoring
# CMAKE_PREFIX_PATH - which is also how a package manager tells CMake where its
# libraries are. A vcpkg or Homebrew-toolchain build has no SDK anywhere near
# it and needs those paths honoured. Default ON: the dangerous case is building
# beside the game, and that is the one that should not need a flag.
option(THPS_P8_GUI_ISOLATED_SDL
       "Ignore CMAKE_PREFIX_PATH when locating SDL3, so an SDK prefix cannot leak in"
       ON)

if(THPS_P8_GUI_ISOLATED_SDL)
  set(_thps_sdl_scope NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH)
else()
  set(_thps_sdl_scope "")
endif()

find_package(SDL3 REQUIRED CONFIG ${_thps_sdl_scope})
find_package(SDL3_image REQUIRED CONFIG ${_thps_sdl_scope})

# --- RmlUi, pinned ----------------------------------------------------------
# A commit, not a tag. Two reasons: a pinned commit is what the licence audit
# records, and this is the exact tree the stylesheets were written against - RmlUi
# has no border-style and no grid, and which properties exist changes between
# revisions, so pinning is what makes the RCSS and the parser agree.
set(THPS_P8_RMLUI_COMMIT 588874489295b5dcd13d9eaa002ea2d73b8addbf)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  rmlui
  GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
  GIT_TAG ${THPS_P8_RMLUI_COMMIT}
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(rmlui)

# RmlUi ships two SDL integration pieces and one convenience event loop. We take
# the first two as project sources and deliberately leave the third: its
# ProcessEvents() calls SDL_PollEvent inside its own translation unit with a
# `default: break;`, so no SDL_EVENT_GAMEPAD_* could ever reach us and the
# launcher could not be driven by a controller. Owning the loop is the whole
# point. Upstream calls these "lightweight starting points"; the files are MIT
# and are credited in NOTICE.
add_executable(thps_p8_gui
  src/main.cpp
  src/launcher_app.cpp
  src/platform.cpp
  src/settings.cpp
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.cpp
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Renderer_SDL.cpp)

# ../ so `common/supported_dumps.h` resolves. The same header, reached the same
# way as the identity worker - a second copy of that table would mean only one of
# the gates was ever tested.
target_include_directories(thps_p8_gui PRIVATE
  ${rmlui_SOURCE_DIR}/Backends
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(thps_p8_gui PRIVATE rmlui_core SDL3::SDL3 SDL3_image::SDL3_image)

# The one file a player needs to launch. By default it asks the existing GUI to
# Play (which naturally falls back to first-run setup); --gui opens the normal
# launcher. Keeping this as a thin executable preserves every existing
# thps_p8* filename and keeps all launch policy in the launcher.
add_executable(Project8Recomp
  src/entry.cpp
  src/platform.cpp)
target_link_libraries(Project8Recomp PRIVATE SDL3::SDL3)

if(WIN32)
  # ShellExecuteA, for "Open save folder". Everything else the Windows side
  # needs is in kernel32, which is linked by default.
  target_link_libraries(thps_p8_gui PRIVATE shell32)
  target_link_libraries(Project8Recomp PRIVATE shell32)
endif()

# The install is a portable directory, so the binary has to find its libraries
# beside itself rather than at the absolute path they happened to occupy on the
# machine that built it. Without this, a release archive runs only on the build
# machine - and does so silently, because the developer's own system copy
# satisfies the link.
#
# BUILD_WITH_INSTALL_RPATH, not just INSTALL_RPATH. This is the same correction
# the identity worker already carries, and it was missing here for
# the same reason it is easy to miss: INSTALL_RPATH applies on `cmake --install`,
# and nothing ever installs this target - `make portable` copies it straight out
# of the build tree, so the shipped binary had no $ORIGIN at all.
#
# On the build machine that was invisible when system SDL3 and SDL3_image
# satisfied the link. It stops being invisible on a Steam Deck, which has
# SDL3 at an older minor version and has never had SDL3_image: the launcher
# would silently prefer the host's SDL3 over the bundled one and then fail
# outright on the image codec. The release ABI gate refuses a staged build in
# this shape rather than letting it travel.
#
# BUILD_RPATH_USE_ORIGIN is kept: it kills absolute build-machine paths in the
# cases BUILD_WITH_INSTALL_RPATH does not cover.
if(UNIX AND NOT APPLE)
  set_target_properties(thps_p8_gui Project8Recomp PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN")
elseif(APPLE)
  set_target_properties(thps_p8_gui Project8Recomp PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path")
endif()

# --- the argv renderer's test ------------------------------------------------
# Standalone and framework-free on purpose: the launcher's other tests are
# Python and drive the built executables, and this one is four pure functions.
# It links nothing but settings.cpp, so it builds and runs in a second and
# cannot be broken by an SDL or RmlUi problem.
#
# The omit-empty rule is what it exists to hold - see settings.h.
add_executable(thps_p8_gui_settings_test
  tests/test_settings.cpp
  src/settings.cpp)
target_compile_features(thps_p8_gui_settings_test PRIVATE cxx_std_20)

enable_testing()
add_test(NAME settings COMMAND thps_p8_gui_settings_test)

# Assets are looked up next to the executable first so an installed portable
# directory is self-contained; this fallback is what makes `gui-run` work
# straight out of the build tree without an install step.
# The source assets/ directory, as a fallback when none is staged beside the
# executable. It is a developer convenience - it lets the build tree run without
# an install step - and it is an absolute path compiled into the binary as a
# string literal, so -ffile-prefix-map cannot reach it and stripping does not
# remove it. A shipped build has assets beside it and never consults this, so
# release packaging turns it off rather than publishing the build machine's
# directory layout.
option(THPS_P8_GUI_EMBED_SOURCE_ASSET_PATH
       "Compile in the source assets/ path as a fallback (developer convenience)"
       ON)

if(THPS_P8_GUI_EMBED_SOURCE_ASSET_PATH)
  set(_thps_gui_source_assets "${CMAKE_CURRENT_SOURCE_DIR}/assets")
else()
  set(_thps_gui_source_assets "assets")
endif()

target_compile_definitions(thps_p8_gui PRIVATE
  # Not optional and not defaulted: both backend headers #error without it, and
  # the .cpp files silently compile their SDL2 branch if it is wrong.
  RMLUI_SDL_VERSION_MAJOR=3
  THPS_P8_GUI_SOURCE_ASSETS="${_thps_gui_source_assets}"
  THPS_P8_GUI_RMLUI_COMMIT="${THPS_P8_RMLUI_COMMIT}")

# The path remapping this needs is set with add_compile_options near the top of
# this file, so that it also covers RmlUi.

# The portable-directory layout the amended distribution shape calls for: the
# binary and its assets sit in one relocatable folder.
#
# A separate always-run target rather than POST_BUILD on the executable. A
# POST_BUILD command only fires when the target itself relinks, so editing only
# an .rml or .rcss left a stale copy staged next to the binary and the program
# rendered the old document while reporting a successful build. Copying is
# cheap; being silently wrong is not.
#
# The destination is removed first because copy_directory only ever adds: a file
# deleted from the source stays in the staged copy indefinitely. That is not
# hypothetical - the window icon changed from PNG to BMP and the dead PNG kept
# being staged, and would have kept being shipped, because nothing ever removes
# it. An asset tree that cannot lose a file is not a copy of the source.
add_custom_target(thps_p8_gui_assets ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:thps_p8_gui>/assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:thps_p8_gui>/assets
  COMMENT "Staging assets/ beside thps_p8_gui")
add_dependencies(thps_p8_gui_assets thps_p8_gui)
