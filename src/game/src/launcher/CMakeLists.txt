# thps_p8_launch - the process supervisor, as a standalone project.
#
# It is also built as a target of the game's CMakeLists, which is convenient
# when you have an SDK and a dump. This file exists because the supervisor's
# whole design property is that it must still build and run when the game does
# not - that is precisely when it is most needed - and a build that can only be
# configured alongside the game does not hold that property in practice.
#
# It links nothing. Standard library only.
#
#   cmake -S src/game/src/launcher -B build/supervisor -G Ninja

cmake_minimum_required(VERSION 3.24)
project(thps_p8_launch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# A console application on Windows, deliberately: it is a CLI wrapper and
# --check --json writes to stdout.
add_executable(thps_p8_launch
  main.cpp
  platform.h
  platform_linux.inc
  platform_windows.inc
  platform_apple.inc
  platform_generic.inc)

# Keep the builder's home directory out of the binary: __FILE__ bakes absolute
# paths into string literals that stripping does not remove. clang-cl ignores
# the GNU spelling with only a warning, so Windows needs the passthrough.
if(MSVC)
  target_compile_options(thps_p8_launch PRIVATE "/clang:-ffile-prefix-map=$ENV{HOME}=.")
  target_link_options(thps_p8_launch PRIVATE "/PDBALTPATH:%_PDB%")
else()
  target_compile_options(thps_p8_launch PRIVATE "-ffile-prefix-map=$ENV{HOME}=.")
endif()
