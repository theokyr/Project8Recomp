# thps_p8_identify - the launcher's SDK-side worker.
#
# Its own project for the opposite reason from the GUI: the GUI must link no
# SDK, and this must link one. Keeping them apart is what lets the GUI stay a UI
# over two subprocess calls.
#
#   cmake -S src/identify -B build/identify -G Ninja \
#         -DTHPS_P8_SDK_ROOT=<path to a rexglue SDK prefix>
cmake_minimum_required(VERSION 3.24)
project(thps_p8_identify LANGUAGES CXX)

# C++23, not 20: rex/types.h uses std::byteswap.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(NOT DEFINED THPS_P8_SDK_ROOT)
  # No default. The SDK lives wherever the builder put it, and guessing a
  # path only produces a confusing failure somewhere else.
  set(THPS_P8_SDK_ROOT "" CACHE PATH "Prebuilt rexglue SDK prefix")
endif()

if(NOT EXISTS "${THPS_P8_SDK_ROOT}/include/rex/filesystem/devices/disc_image_device.h")
  message(FATAL_ERROR
    "rexglue SDK not found at ${THPS_P8_SDK_ROOT}.\n"
    "Pass -DTHPS_P8_SDK_ROOT=<prefix> pointing at the same SDK the game builds against.")
endif()

add_executable(thps_p8_identify main.cpp)
# `..` so the shared dump table is reached as "common/supported_dumps.h", the
# same spelling the game's gate uses. One table, one include path, two gates.
target_include_directories(thps_p8_identify PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${THPS_P8_SDK_ROOT}/include")

# The runtime, matching this build's configuration - and it has to match,
# because only one copy may end up beside the executables in the portable
# directory. The SDK suffixes its libraries by config (none / rd / d), and a
# binary that links a different suffix than the game does drags a second copy of
# the whole runtime into the install, each with its own kernel-state singleton.
# That is not a theoretical tidiness problem: it is a null dereference the first
# time the two disagree about which one is initialised.
#
# Named by hand rather than via a CMake package because the SDK prefix ships
# none, and the file naming differs per platform: an import library on Windows,
# a shared object or dylib elsewhere.
set(_thps_rt_suffix "")
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(_thps_rt_suffix "rd")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_thps_rt_suffix "d")
endif()

if(WIN32)
  set(_thps_rt "${THPS_P8_SDK_ROOT}/lib/rexruntime${_thps_rt_suffix}.lib")
elseif(APPLE)
  set(_thps_rt "${THPS_P8_SDK_ROOT}/lib/librexruntime${_thps_rt_suffix}.dylib")
else()
  set(_thps_rt "${THPS_P8_SDK_ROOT}/lib/librexruntime${_thps_rt_suffix}.so")
endif()

if(NOT EXISTS "${_thps_rt}")
  message(FATAL_ERROR
    "rexglue runtime not found at ${_thps_rt}.\n"
    "The SDK prefix must carry the ${CMAKE_BUILD_TYPE} configuration, because the\n"
    "game and this tool have to load the same one.")
endif()

target_link_libraries(thps_p8_identify PRIVATE "${_thps_rt}")

# Keep the builder's home directory out of the binary: __FILE__ bakes absolute
# paths into string literals that stripping does not remove. clang-cl ignores
# the GNU spelling with only a warning, so Windows needs the passthrough.
if(MSVC)
  target_compile_options(thps_p8_identify PRIVATE
    "/clang:-ffile-prefix-map=$ENV{HOME}=."
    "/clang:-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/..=.")
else()
  target_compile_options(thps_p8_identify PRIVATE
    "-ffile-prefix-map=$ENV{HOME}=."
    "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/..=.")
endif()

# Both of these libraries are unresolvable next to the exe without a search
# path, so the install rpath points at the executable's own directory. Windows
# has no rpath - it looks beside the exe already - so this is a no-op there.
# $ORIGIN in the built binary, not just the installed one.
#
# BUILD_RPATH pointing at the SDK prefix meant the binary copied out of the
# build tree - which is exactly what release packaging copies - resolved its
# runtime from the SDK cache of whoever built it. It worked on that machine and
# nowhere else, and it tested as working there for the same reason.
#
# So the build tree gets the shipped rpath. Running from the build tree now
# needs the runtime staged beside it or on LD_LIBRARY_PATH, which is a cost
# worth paying to stop a build-machine path reaching a user.
if(NOT WIN32)
  set_target_properties(thps_p8_identify PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH $<IF:$<BOOL:${APPLE}>,@loader_path,$ORIGIN>)
endif()
