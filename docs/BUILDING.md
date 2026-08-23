# Building from source

Most people do not need this. The releases carry prebuilt binaries; you only
need a compiler if you want to change something.

There are two very different things to build.

## The launcher

Builds anywhere, in about a minute. It links no part of the SDK and needs no
copy of the game, which is why CI can build it and why it is a reasonable first
thing to try.

**Dependencies:** CMake 3.24+, Ninja, a C++20 compiler, SDL3, SDL3_image,
FreeType. RmlUi is fetched by CMake at a pinned commit.

```sh
# Arch
sudo pacman -S cmake ninja sdl3 sdl3_image freetype2
# Debian/Ubuntu — SDL3 may not be packaged yet; see tools/ci/build_sdl3.sh
sudo apt install cmake ninja-build libfreetype-dev
# macOS
brew install cmake ninja sdl3 sdl3_image freetype

cmake -S src/launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher
ctest --test-dir build/launcher --output-on-failure
```

## The game

This is the part that needs your own copy, and there is no way around it: static
recompilation translates the game's PowerPC code into C++, so the recompiler
reads `default.xex` from your disc image as its input. Nothing in this
repository substitutes for it.

You will need:

1. **The rexglue SDK**, built with the patches in `patches/rexglue-sdk/`.
   Apply them in the machine-readable order in `patches/rexglue-sdk/series` —
   it is not numeric order, and the README says why. Without them the game runs about
   four times slower, and on macOS it does not run at all.
2. **Your extracted game data**, which the launcher produces, or which you can
   extract yourself with `thps_p8_identify --identify_disc=... --extract_to=...`.
3. **Codegen**, run against your `default.xex` using `config/thps_p8_manifest.toml`.
   Replace `REPLACE_ME` in it with the path to your own tree first.

```sh
rexglue codegen config/thps_p8_manifest.toml

cmake -S src/game -B build/game -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/path/to/your/patched/sdk/prefix
cmake --build build/game
```

The generated sources are not in this repository and never will be: they are a
mechanical translation of a copyrighted binary. They are regenerated on your
machine from configuration that is committed here — addresses, sizes, names and
table entries, which are facts about a binary rather than the binary.

## Building for Windows, from Linux

You do not need a Windows machine. You do need clang, LLVM's binutils, and
Microsoft's headers and import libraries.

The reason it has to be clang targeting the MSVC ABI, rather than mingw: the
ReXGlue SDK exports MSVC-mangled C++ with MSVC standard-library types in its
interface, so mingw-w64 cannot link against it under any combination of flags.

```sh
# Fetches Microsoft's CRT and Windows SDK, and fixes the header casing that a
# case-sensitive filesystem trips over.
tools/cross/setup_windows_sysroot.sh

# The SDK is built from source here rather than taken prebuilt, so the runtime
# patches in patches/ are actually in the result - including the save fixes.
cmake -S src/game -B build/game-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=tools/cross/windows-msvc.cmake \
  -DREXSDK_DIR=/path/to/patched/rexglue-sdk \
  -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/game-win
```

**Vulkan, not D3D12, and deliberately.** D3D12 is the SDK's default on Windows,
and enabling it pulls in `thirdparty/dxbc/DXBCChecksum.cpp`, which is
AMD-copyrighted and whose redistribution terms this project has not resolved.
Vulkan avoids that file, and is the backend actually tested on Linux and macOS.

Windows builds are not tested on Windows hardware — see
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## The disc identity worker

Needs the SDK, because reading a disc image is exactly the thing the launcher is
not allowed to do itself.

```sh
cmake -S src/identify -B build/identify -G Ninja \
  -DTHPS_P8_SDK_ROOT=/path/to/your/sdk/prefix
cmake --build build/identify
```

## Putting a portable install together

Stage the four binaries, the SDK's runtime libraries and the GPU plugin, and the
launcher's `assets/` into one folder. The GPU plugin is the one people forget:
without `librexgpu-xenos*`, the game starts, opens a window and draws nothing.

## Before publishing anything

```sh
tools/check_no_game_content.sh
python3 tools/check_linux_abi.py <staged Linux directory>
tools/check_gpl_boundary.sh <every binary you are about to ship>
tools/make_notice.py --sdk /path/to/rexglue-sdk > NOTICE
```
