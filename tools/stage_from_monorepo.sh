#!/usr/bin/env bash
# Copy the port's source out of the development monorepo into this repository.
#
# This repo is a checkpointed mirror. Cut a release by running this, reviewing
# the diff, and committing.
#
#   tools/stage_from_monorepo.sh /path/to/game-preservation

set -euo pipefail

SRC_REPO="${1:-/home/theo/src/game-preservation}"
GAME="$SRC_REPO/games/tony-hawks-project-8"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -d "$GAME/recomp" ]; then
  echo "stage: no recomp tree at $GAME" >&2
  exit 1
fi

say() { printf '==> %s\n' "$*"; }

say "launcher"
mkdir -p "$HERE/src/launcher"
rsync -a --delete --exclude 'out/' --exclude '__pycache__/' --exclude 'TODO.md' \
  "$GAME/recomp/gui/" "$HERE/src/launcher/"

say "identify"
mkdir -p "$HERE/src/identify"
rsync -a --delete --exclude 'out/' "$GAME/recomp/identify/" "$HERE/src/identify/"

say "common"
mkdir -p "$HERE/src/common"
rsync -a --delete "$GAME/recomp/common/" "$HERE/src/common/"

# src/ and the CMake graph only. generated/ is the recompiler's output — a
# translation of the user's own executable, regenerated locally.
say "game host"
mkdir -p "$HERE/src/game"
rsync -a --delete "$GAME/recomp/build/src/" "$HERE/src/game/src/"
cp "$GAME/recomp/build/CMakeLists.txt" "$HERE/src/game/CMakeLists.txt"
cp "$GAME/recomp/build/CMakePresets.json" "$HERE/src/game/CMakePresets.json" 2>/dev/null || true

say "recompiler config"
mkdir -p "$HERE/config"
rsync -a --delete --exclude 'generated/' --exclude '*.local.toml' \
  "$GAME/recomp/config/" "$HERE/config/"
cp "$GAME/recomp/recomp.yml" "$HERE/config/recomp.yml" 2>/dev/null || true

say "runtime patches"
mkdir -p "$HERE/patches"
rsync -a --delete "$GAME/recomp/patches/rexglue-sdk/" "$HERE/patches/rexglue-sdk/"

say "done"
echo
echo "Before committing a release:"
echo "  tools/check_no_game_content.sh"
echo "  tools/make_notice.py --sdk <sdk> > NOTICE"
