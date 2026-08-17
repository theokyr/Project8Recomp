#!/usr/bin/env bash
# Copy a macOS binary's non-system dylibs next to it and rewrite the paths.
#
# Homebrew links by absolute path (/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib),
# so a binary built on a runner with Homebrew SDL3 runs only on a machine that
# has the same Homebrew SDL3 in the same place. The archive has to carry them.
#
#   tools/ci/bundle_macos.sh <dir-with-binaries> <binary> [binary...]

set -euo pipefail

OUT="$1"; shift
[ "$#" -gt 0 ] || { echo "usage: $0 <out-dir> <binary>..." >&2; exit 2; }

# Anything outside these prefixes ships with the archive. /usr/lib and
# /System are part of macOS and must NOT be copied.
is_system() {
  case "$1" in
    /usr/lib/*|/System/*) return 0 ;;
    *) return 1 ;;
  esac
}

deps_of() {
  otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}'
}

# Breadth-first over the dependency graph: SDL3_image pulls libpng, which pulls
# zlib. Copying only the direct dependencies leaves the second hop dangling.
pending=("$@")
seen=""
while [ "${#pending[@]}" -gt 0 ]; do
  current="${pending[0]}"
  pending=("${pending[@]:1}")

  for dep in $(deps_of "$current"); do
    is_system "$dep" && continue
    case "$dep" in @rpath/*|@loader_path/*) continue ;; esac

    base=$(basename "$dep")
    case " $seen " in *" $base "*) continue ;; esac
    seen="$seen $base"

    if [ ! -f "$OUT/$base" ]; then
      cp -L "$dep" "$OUT/$base" 2>/dev/null || continue
      chmod u+w "$OUT/$base"
      echo "bundled $base"
    fi
    pending+=("$OUT/$base")
  done
done

# Rewrite every reference to the copied names, and give each binary an rpath
# pointing at its own directory.
for bin in "$@" "$OUT"/*.dylib; do
  [ -f "$bin" ] || continue
  for dep in $(deps_of "$bin"); do
    is_system "$dep" && continue
    case "$dep" in @rpath/*|@loader_path/*) continue ;; esac
    base=$(basename "$dep")
    [ -f "$OUT/$base" ] || continue
    install_name_tool -change "$dep" "@loader_path/$base" "$bin" 2>/dev/null || true
  done
  install_name_tool -add_rpath "@loader_path" "$bin" 2>/dev/null || true
done

# A dylib also names itself; if that stays absolute, the loader can still be
# sent back to the Homebrew copy.
for lib in "$OUT"/*.dylib; do
  [ -f "$lib" ] || continue
  install_name_tool -id "@loader_path/$(basename "$lib")" "$lib" 2>/dev/null || true
done

# Drop absolute rpaths. Everything in the archive lives in one folder, so
# @loader_path is the only search path any of it needs - and an entry naming the
# SDK prefix of whoever built this is both a directory the user does not have
# and a path they should not be shown.
for bin in "$@" "$OUT"/*.dylib; do
  [ -f "$bin" ] || continue
  otool -l "$bin" 2>/dev/null | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}' | while read -r rp; do
    case "$rp" in
      @loader_path*|@executable_path*) ;;
      *) install_name_tool -delete_rpath "$rp" "$bin" 2>/dev/null || true ;;
    esac
  done
done

echo "--- resulting links ---"
for bin in "$@"; do
  [ -f "$bin" ] || continue
  echo "$bin:"; otool -L "$bin" | tail -n +2
done
