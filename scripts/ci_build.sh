#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root/.."
west init -l "$root"
west update
west zephyr-export
python3 -m pip install -r zephyr/scripts/requirements-base.txt PyYAML
"$root/scripts/build.sh" ethercat -p always
mkdir -p "$root/dist"
cp "$root/build/ethercat_slave/zephyr/zephyr.bin" "$root/dist/dust-adapter-ethercat.bin"
cp "$root/build/ethercat_slave/zephyr/zephyr.elf" "$root/dist/dust-adapter-ethercat.elf"
cp "$root/build/ethercat_slave/zephyr/.config" "$root/dist/build.config"
cp "$root/apps/ethercat_slave/esi/DUST_HPM5E_FreeRun.xml" "$root/dist/"
west manifest --freeze > "$root/dist/west-frozen.yml"
git -C "$root" rev-parse HEAD > "$root/dist/COMMIT"
cp "$root/LICENSE" "$root/NOTICE" "$root/external/ssc/README.md" "$root/dist/"
(cd "$root/dist" && sha256sum dust-adapter-ethercat.bin dust-adapter-ethercat.elf DUST_HPM5E_FreeRun.xml build.config west-frozen.yml COMMIT LICENSE NOTICE README.md > SHA256SUMS)
