#!/usr/bin/env bash
# Stage a complete, playable portable install and archive it.
#
# This is the release artifact: unpack, run the launcher, point it at your own
# disc image, play. It carries the runtime and no files copied from the disc.
#
#   tools/package_release.sh --platform linux-x86_64 --version v0.2.0 \
#     --game <dir with thps_p8> --launcher <dir with thps_p8_gui> \
#     --identify <dir with thps_p8_identify> --sdk <sdk prefix>
#
# Run tools/check_gpl_boundary.sh on the result before publishing.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLATFORM=""; VERSION=""; GAME_DIR=""; LAUNCHER_DIR=""; IDENTIFY_DIR=""; SDK_DIR=""
OUT_DIR="$HERE/dist"

while [ $# -gt 0 ]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --version)  VERSION="$2"; shift 2 ;;
    --game)     GAME_DIR="$2"; shift 2 ;;
    --launcher) LAUNCHER_DIR="$2"; shift 2 ;;
    --identify) IDENTIFY_DIR="$2"; shift 2 ;;
    --sdk)      SDK_DIR="$2"; shift 2 ;;
    --out)      OUT_DIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

for required in PLATFORM VERSION GAME_DIR LAUNCHER_DIR IDENTIFY_DIR SDK_DIR; do
  [ -n "${!required}" ] || { echo "missing --${required,,}" >&2; exit 2; }
done

case "$PLATFORM" in
  windows-x86_64) EXE=".exe"; LIBGLOB="*.dll" ;;
  macos-arm64)    EXE="";     LIBGLOB="*.dylib" ;;
  linux-x86_64)   EXE="";     LIBGLOB="*.so*" ;;
  *) echo "unsupported --platform: $PLATFORM" >&2; exit 2 ;;
esac

if [[ ! "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
  echo "invalid --version: $VERSION" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd -P)"

NAME="Project8Recomp-${VERSION}-${PLATFORM}"
STAGE="$OUT_DIR/$NAME"
ARCHIVE="$OUT_DIR/$NAME.zip"
rm -rf -- "$STAGE"
rm -f -- "$ARCHIVE"
mkdir -p "$STAGE"

say() { printf '==> %s\n' "$*"; }

copy_one() {
  local src="$1" what="$2"
  [ -f "$src" ] || { echo "missing $what: $src" >&2; exit 1; }
  cp "$src" "$STAGE/"
}

say "binaries"
copy_one "$GAME_DIR/thps_p8$EXE"              "the game"
copy_one "$GAME_DIR/thps_p8_launch$EXE"       "the supervisor"
copy_one "$LAUNCHER_DIR/thps_p8_gui$EXE"      "the launcher"
copy_one "$LAUNCHER_DIR/Project8Recomp$EXE"   "the player entry point"
copy_one "$IDENTIFY_DIR/thps_p8_identify$EXE" "the disc worker"

say "runtime libraries"
# Read what the game actually links rather than copying every runtime in the
# prefix. The SDK ships three configs side by side (Release, RelWithDebInfo and
# Debug), and globbing them all added 134 MB of libraries that nothing loads.
needed_libs() {
  local bin="$1"
  case "$PLATFORM" in
    windows*)
      if command -v llvm-objdump >/dev/null 2>&1; then
        llvm-objdump -p "$bin" 2>/dev/null | awk '/DLL Name:/ {print $3}'
      elif command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
        x86_64-w64-mingw32-objdump -p "$bin" 2>/dev/null | awk '/DLL Name:/ {print $3}'
      else
        objdump -p "$bin" 2>/dev/null | awk '/DLL Name:/ {print $3}'
      fi
      ;;
    macos*)   otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}' \
                | xargs -n1 basename 2>/dev/null ;;
    *)        objdump -p "$bin" 2>/dev/null | awk '/NEEDED/ {print $2}' ;;
  esac
}

rt_name=""
for lib in $(needed_libs "$GAME_DIR/thps_p8$EXE"); do
  case "$lib" in
    *rexruntime*|*TracyClient*|*Tracy*)
      for dir in "$SDK_DIR/lib" "$SDK_DIR/bin"; do
        [ -f "$dir/$lib" ] && cp -L "$dir/$lib" "$STAGE/" && break
      done
      case "$lib" in *rexruntime*) rt_name="$lib" ;; esac
      ;;
  esac
done
[ -n "$rt_name" ] || { echo "the game links no rexglue runtime - wrong --game dir?" >&2; exit 1; }

# Tracy is linked by the instrumented runtime, not necessarily by the game
# executable itself. Looking only at the top-level import table produced a ZIP
# that contained rexruntimerd but omitted TracyClientrd, so the loader failed
# before main on the exact configuration used for performance verification.
for lib in $(needed_libs "$STAGE/$rt_name"); do
  case "$lib" in
    *TracyClient*|*Tracy*)
      found_tracy=0
      for dir in "$SDK_DIR/lib" "$SDK_DIR/bin"; do
        if [ -f "$dir/$lib" ]; then
          cp -L "$dir/$lib" "$STAGE/"
          found_tracy=1
          break
        fi
      done
      [ "$found_tracy" -eq 1 ] || {
        echo "runtime dependency missing from SDK prefix: $lib" >&2
        exit 1
      }
      ;;
  esac
done

# The GPU plugin is dlopen'd by name, so it is not in the link table and is the
# one people forget. Without it the game starts, opens a window, runs, and draws
# nothing - which reads as a broken port rather than a missing file.
#
# It has to match the runtime's build config: loading a Release plugin beside a
# RelWithDebInfo runtime means two copies of the SDK in one process, each with
# its own kernel-state singleton, and the second one to initialise is null when
# the first is used.
suffix="$(printf '%s' "$rt_name" | sed -E 's/.*rexruntime([a-z]*)\..*/\1/')"
found_gpu=0
for dir in "$SDK_DIR/lib" "$SDK_DIR/bin"; do
  for lib in "$dir"/*rexgpu-xenos"$suffix".*; do
    [ -f "$lib" ] || continue
    # Runtime artifacts only. A `.lib` beside a `.dll` on Windows is an import
    # library - a build-time file that does nothing in a release but invite the
    # question of why it is there.
    case "$lib" in *.lib|*.a|*.exp|*.pdb) continue ;; esac
    cp -L "$lib" "$STAGE/"; found_gpu=1
  done
done
[ "$found_gpu" -eq 1 ] || { echo "no GPU plugin matching '$suffix' - the game would draw nothing" >&2; exit 1; }

# SDL3 and friends, for the launcher, when they are not system libraries.
for lib in "$LAUNCHER_DIR"/$LIBGLOB; do
  [ -f "$lib" ] && cp -L "$lib" "$STAGE/"
done

if [ "$PLATFORM" = "windows-x86_64" ]; then
  # /MD is required by the MSVC-ABI SDK and launcher dependencies. Requiring
  # the app-local runtime here is what makes the ZIP work on a clean Windows
  # machine instead of only on a developer machine (or Proton, which supplies
  # compatible built-ins and otherwise hides the omission).
  for crt in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do
    [ -f "$STAGE/$crt" ] || {
      echo "missing app-local Visual C++ runtime: $crt" >&2
      exit 1
    }
  done
fi

# macOS links its dependencies by absolute path, so a Homebrew-built launcher
# names /opt/homebrew/... and runs only on a machine with the same Homebrew
# packages installed. Copy the graph in and rewrite the paths.
if [ "${PLATFORM#macos}" != "$PLATFORM" ]; then
  say "bundling macOS dependencies"
  bash "$HERE/tools/ci/bundle_macos.sh" "$STAGE" \
    "$STAGE/thps_p8_gui" "$STAGE/thps_p8" "$STAGE/thps_p8_identify" "$STAGE/thps_p8_launch" \
    > /dev/null
  # An absolute Homebrew or local build path left in any binary means the
  # archive works here and nowhere else - the exact failure this step exists to
  # prevent, and one that is invisible on the machine that built it.
  if otool -L "$STAGE"/* 2>/dev/null | grep -qE "/opt/homebrew|/usr/local/Cellar"; then
    echo "REFUSING: a binary still references a Homebrew path" >&2
    otool -L "$STAGE"/* 2>/dev/null | grep -E "/opt/homebrew|/usr/local/Cellar" | head >&2
    exit 1
  fi
fi

say "assets and notices"
# The launcher reads its UI from these files at runtime, so they are as much
# "the build" as the executable is - a stale stylesheet ships a stale layout
# next to a current binary, and the binary cannot tell.
#
# They come from this repository, and the launcher was built from a tree that
# staged its own copy beside the executable. If those disagree, the repository
# has not been re-staged since the launcher was built and the archive would ship
# a UI nobody tested. Refuse rather than guess which is newer.
if [ -d "$LAUNCHER_DIR/assets" ]; then
  if ! diff -r "$HERE/src/launcher/assets" "$LAUNCHER_DIR/assets" >/dev/null 2>&1; then
    echo "REFUSING: the UI assets in this repository differ from the ones the" >&2
    echo "launcher was built with. Re-stage the source, rebuild, and try again." >&2
    diff -rq "$HERE/src/launcher/assets" "$LAUNCHER_DIR/assets" 2>&1 | head -5 >&2
    exit 1
  fi
fi
cp -r "$HERE/src/launcher/assets" "$STAGE/"
cp "$HERE/LICENSE" "$HERE/NOTICE" "$HERE/CHANGELOG.md" "$STAGE/"

cat > "$STAGE/README.txt" <<EOF
Tony Hawk's Project 8 - native port (Project8Recomp)

You need your own copy of the game. This folder contains no files copied from
the disc.

  1. Run:  Project8Recomp${EXE}
  2. Point it at a disc image of your own copy.
  3. It checks the disc, copies the game here, and starts it.

After setup, Project8Recomp${EXE} starts the game through the launcher. Run it
with --gui to open the full launcher instead. On Steam Deck, add this one file
as a Non-Steam Game, use the native Linux ZIP, and do not force Proton.

The existing thps_p8* executable names are retained for compatibility with
v0.1.0 installs and shortcuts.

Everything stays in this folder - game data, saves, settings. Move it, copy it
between your own machines, or delete it; nothing is written anywhere else.

Licences for everything this is built from are in NOTICE.
Full documentation: https://github.com/theokyr/Project8Recomp
EOF

say "stripping debug symbols"
# Consistency, not just size. Debug info is inline in ELF and external on the
# other two (.pdb, .dSYM), so shipping "the same build" three ways otherwise
# means the Linux archive carries ~117 MB of DWARF the others do not - 93 MB
# against 17 MB, for identical code.
#
# Nothing here needs symbols to run, and a stack trace from a stranger is not
# worth 75 MB on every download. Rebuild locally when one is actually needed.
case "$PLATFORM" in
  macos*)
    for f in "$STAGE"/Project8Recomp "$STAGE"/thps_p8 "$STAGE"/thps_p8_* "$STAGE"/*.dylib; do
      if [ -f "$f" ]; then
        strip -x "$f" 2>/dev/null || true
      fi
    done
    ;;
  windows*)
    # Debug info already lives in the .pdb files, which are not staged. The
    # executables have nothing to strip.
    ;;
  *)
    for f in "$STAGE"/Project8Recomp "$STAGE"/thps_p8 "$STAGE"/thps_p8_* "$STAGE"/*.so*; do
      if [ -f "$f" ]; then
        objcopy --strip-debug "$f" 2>/dev/null || true
      fi
    done
    ;;
esac

say "normalising library search paths"
# The SDK's own shared libraries carry the rpath its build tree had - one of
# them names an absolute directory on the machine that built the SDK. Beside the
# fact that it leaks a path, it is a directory a user does not have, and a
# library that finds nothing there is one lookup away from finding the wrong
# thing somewhere else. Everything in this archive sits in one folder, so the
# only search path any of it needs is its own.
case "$PLATFORM" in
  linux*)
    if command -v patchelf >/dev/null 2>&1; then
      for lib in "$STAGE"/*.so*; do
        [ -f "$lib" ] || continue
        # Literal loader token, not a shell variable.
        # shellcheck disable=SC2016
        patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
      done
    else
      echo "patchelf not found - cannot normalise rpaths; install it and re-run" >&2
      exit 1
    fi
    ;;
esac

say "refusing to ship the build machine"
# Two ways a release ends up working only where it was built, both invisible on
# that machine because the paths resolve there:
#
#   - an rpath naming the builder's SDK prefix, so the binary loads its runtime
#     from a directory no user has
#   - absolute source paths baked into string literals by __FILE__
#
# The first is a functional failure, not a cosmetic one, and it passed a
# "unpack it somewhere clean and run it" test on the build machine.
leaked=0
for f in "$STAGE"/*; do
  [ -f "$f" ] || continue
  # Binaries only. objdump and strings both fail on LICENSE and NOTICE, and
  # under `set -o pipefail` that failure ends the script rather than the file.
  case "$(file -bL "$f" 2>/dev/null)" in
    *ELF*|*Mach-O*|*PE32*) ;;
    *) continue ;;
  esac
  case "$PLATFORM" in
    linux*)
      # Every entry must be relative to the binary's own directory. Empty
      # entries are padding and harmless; anything absolute is a directory that
      # belongs to whoever built this.
      rp=$(objdump -p "$f" 2>/dev/null | awk '/RUNPATH|RPATH/ {print $2}')
      old_ifs=$IFS; IFS=':'
      for entry in ${rp:-}; do
        # Literal loader tokens, not shell variables.
        # shellcheck disable=SC2016
        case "$entry" in
          ""|'$ORIGIN'|'$ORIGIN'/*) ;;
          *) echo "!! $(basename "$f") has rpath entry '$entry' outside \$ORIGIN" >&2; leaked=1 ;;
        esac
      done
      IFS=$old_ifs
      ;;
  esac
  if strings "$f" 2>/dev/null | grep -qE "$(printf '/home/[a-z]|/Users/[a-z]')"; then
    echo "!! $(basename "$f") contains an absolute home-directory path" >&2
    strings "$f" 2>/dev/null | grep -oE "(/home|/Users)/[A-Za-z0-9._-]+[^\"' ]*" | sort -u | head -3 >&2
    leaked=1
  fi
done
[ "$leaked" -eq 0 ] || { echo "REFUSING: the archive would only work where it was built" >&2; exit 1; }

say "refusing to ship anything from the disc"
# The archive is the last place this can be caught, and the only place it is
# checked against what is actually being published rather than what is tracked.
bad=0
while IFS= read -r f; do
  case "$f" in
    *.xex|*.xzp|*.pak|*.bik|*.bik.xen|*.xma|*.fsb|*.iso|*/DATA/*)
      echo "!! $f" >&2; bad=1 ;;
  esac
done < <(find "$STAGE" -type f)
[ "$bad" -eq 0 ] || { echo "REFUSING: game content in the archive" >&2; exit 1; }

say "writing provenance and checksums"
cat > "$STAGE/BUILDINFO.txt" <<EOF
Project8Recomp release: $VERSION
Platform: $PLATFORM
Source commit: $(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)
EOF

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

while IFS= read -r relative; do
  relative="${relative#./}"
  printf '%s  %s\n' "$(hash_file "$STAGE/$relative")" "$relative"
done < <(cd "$STAGE" && find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort) \
  > "$STAGE/SHA256SUMS"

say "archiving"
( cd "$OUT_DIR" && zip -qr "$NAME.zip" "$NAME" )
echo "$ARCHIVE"

say "done"
# Human-oriented summary; release filenames are controlled above.
# shellcheck disable=SC2012
ls -la "$STAGE" | head -20
echo
echo "Next: tools/check_gpl_boundary.sh $STAGE"
