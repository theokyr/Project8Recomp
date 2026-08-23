#!/usr/bin/env bash
# Keep the SDK patch inventory and its machine-readable apply order in sync.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$HERE/patches/rexglue-sdk"
SERIES="$PATCH_DIR/series"

duplicates="$(sed '/^[[:space:]]*$/d' "$SERIES" | sort | uniq -d)"
if [ -n "$duplicates" ]; then
  echo "duplicate patch names in patches/rexglue-sdk/series:" >&2
  printf '%s\n' "$duplicates" >&2
  exit 1
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
actual="$(printf '%s\n' "${patches[@]##*/}" | sort)"
listed="$(sed '/^[[:space:]]*$/d' "$SERIES" | sort)"

if ! diff -u <(printf '%s\n' "$actual") <(printf '%s\n' "$listed"); then
  echo "patch inventory and series differ" >&2
  exit 1
fi

echo "clean: $(printf '%s\n' "$listed" | wc -l) patches listed exactly once"
