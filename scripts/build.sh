#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 usb [extra west build arguments]" >&2
  exit 2
fi
transport="$1"
shift
case "$transport" in
  usb) app=usb_gateway; board=dust-hpm6750 ;;
  *) echo "Unknown transport: $transport" >&2; exit 2 ;;
esac
# No implicit flashing or download. Invoke in an initialized Zephyr workspace.
exec "${WEST:-west}" build -s "$root/apps/$app" -b "$board" \
  -d "${BUILD_DIR:-$root/build/$app}" "$@"
