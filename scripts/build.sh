#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 ethercat [extra west build arguments]" >&2
  exit 2
fi
transport="$1"
shift
case "$transport" in
  ethercat) app=ethercat_slave; board=dust-hpm5e3y ;;
  *) echo "Unknown transport: $transport" >&2; exit 2 ;;
esac
# No implicit flashing or download. Invoke in an initialized Zephyr workspace.
exec "${WEST:-west}" build -s "$root/apps/$app" -b "$board" \
  -d "${BUILD_DIR:-$root/build/$app}" "$@"
