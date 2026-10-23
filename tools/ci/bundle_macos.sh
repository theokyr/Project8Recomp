#!/usr/bin/env bash
# Bundle the complete Mach-O dependency graph. The caller signs the result after
# its final mutations (including stripping), as in tools/package_release.sh.
set -euo pipefail

exec python3 "$(dirname "${BASH_SOURCE[0]}")/bundle_macos.py" "$@"
