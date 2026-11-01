#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ensure src/drivers stays hardware-only (no ecat/protocol dependencies)."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "src" / "drivers"
IO = ROOT / "src" / "io"
INCLUDE = re.compile(r'#include\s*[<"]([^>"]+)[>"]')

# 驱动层不得依赖 EtherCAT/协议/安全策略层。
FORBIDDEN_PREFIXES = ("ecat/", "protocol/", "gateway")
FORBIDDEN_NAMES = {
    "applInterface.h",
    "sampleappl.h",
    "ecat_def.h",
    "gateway_config.h",
    "firmware_can_profile.hpp",
    "io_policy.hpp",
    "latest_can.hpp",
}


def scan(layer: Path, allowed_prefixes: tuple[str, ...], forbidden_names: set[str]) -> list[str]:
    problems = []
    for path in sorted(layer.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        for number, line in enumerate(
            path.read_text(errors="ignore").splitlines(), 1
        ):
            match = INCLUDE.search(line)
            if not match:
                continue
            include = match.group(1)
            if include.startswith(allowed_prefixes):
                continue
            if include.startswith("ecat/") or include in forbidden_names:
                problems.append(f"{path.relative_to(ROOT)}:{number}: {include}")
    return problems


def main() -> int:
    problems = []
    # drivers 不得依赖协议/适配层。
    problems += scan(DRIVERS, ("drivers/",), FORBIDDEN_NAMES)
    # io 适配层可以依赖 protocol/drivers，但不得依赖 ecat。
    problems += scan(IO, ("io/", "protocol/", "drivers/"), {"gateway.h", "runtime.h"})
    if problems:
        for problem in problems:
            print(f"FORBIDDEN: {problem}")
        return 1
    print("OK: src/drivers has no ecat/protocol dependency")
    print("OK: src/io has no ecat dependency")
    return 0


if __name__ == "__main__":
    sys.exit(main())
