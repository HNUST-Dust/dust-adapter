#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ensure src/drivers stays hardware-only (no ecat/protocol dependencies)."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "src" / "drivers"
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


def main() -> int:
    problems = []
    for path in sorted(DRIVERS.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        for number, line in enumerate(
            path.read_text(errors="ignore").splitlines(), 1
        ):
            match = INCLUDE.search(line)
            if not match:
                continue
            include = match.group(1)
            # 同层驱动互相包含是允许的。
            if include.startswith("drivers/"):
                continue
            if include.startswith(FORBIDDEN_PREFIXES) or (
                include in FORBIDDEN_NAMES
            ):
                problems.append(
                    f"{path.relative_to(ROOT)}:{number}: {include}"
                )
    if problems:
        for problem in problems:
            print(f"FORBIDDEN: {problem}")
        return 1
    print("OK: src/drivers has no ecat/protocol dependency")
    return 0


if __name__ == "__main__":
    sys.exit(main())
