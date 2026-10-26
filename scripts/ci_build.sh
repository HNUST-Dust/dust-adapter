#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
transport="${1:-ethercat}"
case "$transport" in
  ethercat) output=ethercat_slave ;;
  usb) output=usb_gateway ;;
  *) echo "Usage: $0 [ethercat|usb]" >&2; exit 2 ;;
esac
cd "$root/.."
west init -l "$root"
west update
west zephyr-export
python3 -m pip install -r zephyr/scripts/requirements-base.txt PyYAML
BUILD_DIR="$root/build/$output" "$root/scripts/build.sh" "$transport" -p always
mkdir -p "$root/dist"
cp "$root/build/$output/zephyr/zephyr.bin" "$root/dist/dust-adapter-$transport.bin"
cp "$root/build/$output/zephyr/zephyr.elf" "$root/dist/dust-adapter-$transport.elf"
cp "$root/build/$output/zephyr/.config" "$root/dist/build.config"
west manifest --freeze > "$root/dist/west-frozen.yml"
git -C "$root" rev-parse HEAD > "$root/dist/COMMIT"
cp "$root/LICENSE" "$root/NOTICE" "$root/config/can_devices.yaml" "$root/dist/"
if [[ "$transport" == ethercat ]]; then
  cp "$root/apps/ethercat_slave/esi/DUST_HPM5E_FreeRun.xml" "$root/dist/"
  cp "$root/external/ssc/README.md" "$root/dist/SSC_README.md"
fi
(cd "$root/dist" && sha256sum dust-adapter-"$transport".bin dust-adapter-"$transport".elf \
  build.config west-frozen.yml COMMIT LICENSE NOTICE can_devices.yaml > SHA256SUMS)
if [[ "$transport" == ethercat ]]; then
  (cd "$root/dist" && sha256sum DUST_HPM5E_FreeRun.xml SSC_README.md >> SHA256SUMS)
fi
