#!/usr/bin/env bash
# Verify no shipped binary links GPL binutils.
#
# The recompiler uses binutils to disassemble; the runtime must not. If this
# fails, fix the link line — do not relicense around it.
#
#   tools/check_gpl_boundary.sh <binary-or-dir> [more...]

set -uo pipefail

PATTERN='bfd_|print_insn_|_bfd_|opcodes_|init_disassemble_info'
fail=0

scan() {
  local f="$1"
  # -L, because staged runtimes are often symlinks into a build tree and `file`
  # would otherwise report "symbolic link to ..." and skip them silently.
  case "$(file -bL "$f" 2>/dev/null)" in
    *ELF*|*Mach-O*|*PE32*) ;;
    *) return ;;
  esac
  local hits
  hits=$( { nm -DC "$f" 2>/dev/null; nm -C "$f" 2>/dev/null; } | grep -Ec "$PATTERN")
  if [ "${hits:-0}" -gt 0 ]; then
    printf '!! %s references binutils symbols (%s matches)\n' "$f" "$hits"
    fail=1
  else
    printf 'ok %s\n' "$f"
  fi
}

[ "$#" -gt 0 ] || { echo "usage: $0 <binary-or-dir>..." >&2; exit 2; }

for target in "$@"; do
  if [ -d "$target" ]; then
    while IFS= read -r f; do scan "$f"; done < <(find "$target" -type f -perm -u+r)
  else
    scan "$target"
  fi
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "REFUSING: a shipped binary links GPL binutils."
  exit 1
fi

echo "clean: no binutils in the shipped link line"
