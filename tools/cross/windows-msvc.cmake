# Cross-compile to the Windows MSVC ABI with clang, using an xwin sysroot.
#
# Needed because the published rexglue SDK exports MSVC-mangled C++ with MSVC
# STL types in its interface, so mingw cannot link against it at all.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Where `xwin splat` put the Microsoft CRT and Windows SDK. Override with
# -DXWIN=<path>; tools/cross/setup_windows_sysroot.sh produces one.
if(NOT DEFINED XWIN)
  set(XWIN "$ENV{HOME}/.cache/thps-p8/xwin-sysroot")
endif()
if(NOT EXISTS "${XWIN}/crt/include/vcruntime.h")
  message(FATAL_ERROR
    "No Windows sysroot at ${XWIN}.\n"
    "Run tools/cross/setup_windows_sysroot.sh, or pass -DXWIN=<path>.")
endif()
set(TARGET_TRIPLE x86_64-pc-windows-msvc)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

# clang-cl takes MSVC-style flags. /imsvc is "system include", which keeps the
# SDK's own warnings out of ours.
# winrt is on the list because the SDK's presenter.h includes <wrl/client.h>,
# which lives under sdk/include/winrt/wrl rather than beside the um headers.
set(_xwin_includes
  "/imsvc${XWIN}/crt/include \
   /imsvc${XWIN}/sdk/include/ucrt \
   /imsvc${XWIN}/sdk/include/um \
   /imsvc${XWIN}/sdk/include/shared \
   /imsvc${XWIN}/sdk/include/winrt")

# -fuse-ld=lld is not optional: without it clang-cl looks for MSVC's link.exe
# and dies with "posix_spawn failed", which reads as a missing compiler rather
# than a missing linker.
# -Wno-unknown-argument because the SDK passes GNU-style flags
# (-ffp-model=strict, -fno-char8_t) that clang-cl ignores, and some of its
# targets build with -Werror - so an ignored flag becomes a hard error.
#
# /Zc:char8_t- is the MSVC-driver spelling of -fno-char8_t. The SDK passes
# the GNU spelling unconditionally, which clang-cl ignores as an unknown
# argument - so u8"" literals stay char8_t and the SDK's own calls that pass
# them to std::string_view stop compiling.
#
# -march=x86-64-v3 matches the SDK's own windows preset. Without it clang
# compiles baseline x86-64, which has no SSSE3, and the SDK's byte-swap
# intrinsics fail with "always_inline function '_mm_shuffle_epi8' requires
# target feature 'ssse3'".
set(CMAKE_C_FLAGS_INIT   "--target=${TARGET_TRIPLE} -fuse-ld=lld -march=x86-64-v3 /MD /Zc:char8_t- -Wno-unknown-argument -Wno-unused-command-line-argument ${_xwin_includes}")
set(CMAKE_CXX_FLAGS_INIT "--target=${TARGET_TRIPLE} -fuse-ld=lld -march=x86-64-v3 /MD /Zc:char8_t- -Wno-unknown-argument -Wno-unused-command-line-argument ${_xwin_includes}")

set(_xwin_libs
  "/libpath:${XWIN}/crt/lib/x86_64 \
   /libpath:${XWIN}/sdk/lib/ucrt/x86_64 \
   /libpath:${XWIN}/sdk/lib/um/x86_64 \
   /libpath:${XWIN}/lib")

# dxgi.lib even with the D3D12 backend disabled: the SDK's UI layer calls
# CreateDXGIFactory1 unconditionally to drive its present tick.
set(_xwin_libs "${_xwin_libs} dxgi.lib")

# 128-bit division helpers. clang lowers `unsigned __int128` division to
# compiler-rt calls, and distributions ship compiler-rt for their own platform
# only - so a Linux host has no Windows copy and the SDK fails to link on one
# divide in its clock code. setup_windows_sysroot.sh builds this from
# msvc_compat_builtins.c.
if(EXISTS "${XWIN}/lib/thps_msvc_compat.lib")
  set(_xwin_libs "${_xwin_libs} thps_msvc_compat.lib")
endif()

set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libs}")

# /MD on the command line as well as the CMake variable: the variable only
# takes effect under CMP0091, and a subproject that does not opt in gets
# clang-cl's default instead - which is the STATIC CRT, and asks the linker
# for libcmt.lib that xwin does not ship.
#
# The release CRT, always. xwin ships no debug CRT because Microsoft does not
# redistribute one, so anything that asks for msvcrtd.lib fails at link with a
# message that looks like a broken sysroot rather than a config choice.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
