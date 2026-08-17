#!/usr/bin/env bash
# Fetch the Microsoft CRT and Windows SDK so Windows binaries can be built on
# Linux, and fix the two things that always bite on a case-sensitive filesystem.
#
# Why this exists: the published rexglue SDK exports MSVC-mangled C++ with MSVC
# STL types in its interface, so mingw-w64 cannot link against it at any
# optimisation level or flag combination. The only route to a Windows build
# without a Windows machine is clang targeting the MSVC ABI, and that needs
# Microsoft's own headers and import libraries.
#
# xwin downloads them from Microsoft's servers under their redistributable
# licence; it accepts that licence on your behalf when you pass --accept-license,
# so read it if that matters to you.
#
#   tools/cross/setup_windows_sysroot.sh

set -euo pipefail

SYSROOT="${SYSROOT:-$HOME/.cache/thps-p8/xwin-sysroot}"
# Cache and sysroot must share a filesystem: xwin moves files into place rather
# than copying, and a cross-device move fails partway leaving an empty sysroot
# that looks like a successful run.
CACHE="${CACHE:-$(dirname "$SYSROOT")/xwin-cache}"

if ! command -v xwin >/dev/null 2>&1; then
  if command -v cargo >/dev/null 2>&1; then
    echo "==> installing xwin"
    cargo install xwin --locked
  else
    echo "xwin is not installed and cargo is not available." >&2
    echo "Install Rust, or install xwin by hand, then re-run." >&2
    exit 1
  fi
fi
XWIN="$(command -v xwin || echo "$HOME/.cargo/bin/xwin")"

for tool in clang-cl lld-link llvm-lib llvm-rc; do
  command -v "$tool" >/dev/null || { echo "missing $tool (install LLVM)" >&2; exit 1; }
done

mkdir -p "$(dirname "$SYSROOT")" "$CACHE"

echo "==> fetching the Microsoft CRT and Windows SDK into $SYSROOT"
rm -rf "$SYSROOT"
"$XWIN" --accept-license --arch x86_64 --cache-dir "$CACHE" splat --output "$SYSROOT"

# xwin lowercases every header. The SDK's own sources include some of them with
# their Windows casing, which is fine on a case-insensitive filesystem and fatal
# here. Symlink the spellings that are actually used.
echo "==> adding case-variant header symlinks"
for name in ObjBase.h SDKDDKVer.h Windows.h WinSock2.h WS2tcpip.h DXProgrammableCapture.h; do
  lower="$(printf '%s' "$name" | tr 'A-Z' 'a-z')"
  for dir in "$SYSROOT/sdk/include/um" "$SYSROOT/sdk/include/shared" "$SYSROOT/sdk/include/ucrt"; do
    if [ -f "$dir/$lower" ] && [ ! -e "$dir/$name" ]; then
      ln -s "$lower" "$dir/$name"
      echo "    $(basename "$dir")/$name"
    fi
  done
done

cat <<EOF

Sysroot ready: $SYSROOT

Build with:
  cmake -S <dir> -B <build> -G Ninja \\
    -DCMAKE_TOOLCHAIN_FILE=tools/cross/windows-msvc.cmake \\
    -DXWIN=$SYSROOT

The cache ($CACHE) is about 1.7 GB and can be deleted once this has run.
EOF
