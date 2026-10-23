#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# No implicit flashing. The repository root is the sole Zephyr application.
exec "${WEST:-west}" build -s "$root" -b dust-hpm6750 \
  -d "${BUILD_DIR:-$root/build}" "$@"
