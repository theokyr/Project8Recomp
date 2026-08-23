#!/usr/bin/env bash
# Refuse to ship anything that came off the disc.
#
# Checks the file listing, not the ignore rules. There is no allowlist.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

fail=0
report() {
  printf '\n!! %s\n' "$1"; shift
  printf '   %s\n' "$@"
  fail=1
}

mapfile -d '' -t publishable < <(git ls-files --cached --others --exclude-standard -z 2>/dev/null)
if [ "${#publishable[@]}" -eq 0 ]; then
  mapfile -d '' -t publishable < <(find . -type f -not -path './.git/*' -print0)
fi

check_glob() {
  local pattern="$1" why="$2"
  local -a hits=()
  # The right-hand side is intentionally a caller-supplied shell glob.
  # shellcheck disable=SC2053
  for f in "${publishable[@]}"; do [[ "$f" == $pattern ]] && hits+=("$f"); done
  if [ "${#hits[@]}" -gt 0 ]; then
    report "$why" "${hits[@]}"
  fi
}

echo "== checking ${#publishable[@]} publishable files (tracked and non-ignored) =="

check_glob '*.xex'      'a game executable is in the tree'
check_glob '*.xzp'      'a disc asset container is in the tree'
check_glob '*.pak'      'a disc asset container is in the tree'
check_glob '*.bik'      'disc video is in the tree'
check_glob '*.bik.xen'  'disc video is in the tree'
check_glob '*.xma'      'disc audio is in the tree'
check_glob '*.fsb'      'disc audio is in the tree'
check_glob '*.iso'      'a disc image is in the tree'
check_glob '*.img'      'a disc image is in the tree'
check_glob 'DATA/*'     'the disc data directory is in the tree'
check_glob '*/DATA/*'   'the disc data directory is in the tree'

# The recompiler's output is a translation of the publisher's machine code.
check_glob '*thps_p8_recomp.*.cpp' 'generated translation units are in the tree'
check_glob '*/generated/*'         'recompiler output is in the tree'

# Backstop: every legitimate file here is source, config or a font.
big=()
for f in "${publishable[@]}"; do
  [ -f "$f" ] || continue
  size=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null || echo 0)
  [ "$size" -gt $((8 * 1024 * 1024)) ] && big+=("$f ($((size / 1024 / 1024)) MB)")
done
if [ "${#big[@]}" -gt 0 ]; then
  report 'a file larger than 8 MB is in the tree' "${big[@]}"
fi

if [ "$fail" -ne 0 ]; then
  echo
  echo "REFUSING: the tree contains something that must not be published."
  exit 1
fi

echo "clean"
