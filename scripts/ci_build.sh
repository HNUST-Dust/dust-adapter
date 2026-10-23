#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root/.."
west init -l "$root"
west update
west zephyr-export
python3 -m pip install -r zephyr/scripts/requirements-base.txt PyYAML
# The application pins module revisions; avoid recursive SDK submodule downloads.
"$root/scripts/build.sh" -p always
mkdir -p "$root/dist"
cp "$root/build/zephyr/zephyr.bin" "$root/dist/dust-adapter-usb.bin"
cp "$root/build/zephyr/zephyr.elf" "$root/dist/dust-adapter-usb.elf"
cp "$root/build/zephyr/.config" "$root/dist/build.config"
west manifest --freeze > "$root/dist/west-frozen.yml"
git -C "$root" rev-parse HEAD > "$root/dist/COMMIT"
cp "$root/LICENSE" "$root/NOTICE" "$root/config/can_devices.yaml" "$root/dist/"
(cd "$root/dist" && sha256sum dust-adapter-usb.bin dust-adapter-usb.elf build.config west-frozen.yml COMMIT LICENSE NOTICE can_devices.yaml > SHA256SUMS)
