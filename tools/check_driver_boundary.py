#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the layered dependency rules of the new firmware architecture.

主约束仍是 CMake target 链接图；本脚本用于捕获"只加 include 不加链接"的
越界引用与旧路径回归。
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
INCLUDE = re.compile(r'#include\s*[<"]([^>"]+)[>"]')

# 每层允许出现的仓库内 include 根。
LAYER_ALLOWED = {
    "common": ("common/",),
    "domain": ("domain/", "common/", "drivers/"),
    "drivers": ("drivers/", "common/"),
    "adapters": ("adapters/", "common/", "domain/", "drivers/"),
    "app": (
        "adapters/",
        "common/",
        "domain/",
        "drivers/",
    ),
}
EXTERNAL_ALLOWED = {
    "common": (),
    "domain": ("pdo_schema.hpp",),
    "drivers": (),
    "adapters": (
        "gateway_config.h",
        "can_profile_generated.hpp",
        "pdo_schema.hpp",
    ),
    "app": ("gateway_config.h", "pdo_schema.hpp", "can_profile_generated.hpp"),
}
# 第三方 / SDK / 生成头前缀（非本仓库层）。
THIRD_PARTY = ("zephyr/", "hpm", "sys/", "device.h", "drivers/")
SSC_NAMES = {
    "coeappl.h",
    "ecat_def.h",
    "ecatappl.h",
    "ecatslv.h",
    "objdef.h",
    "sdoserv.h",
    "sampleappl.h",
    "applInterface.h",
    "eeprom.h",
    "esc.h",
    "mailbox.h",
    "ecatcoe.h",
}
REPO_ROOTS = (
    "common",
    "domain",
    "drivers",
    "adapters",
    "app",
    "platform",
    "ecat",
    "protocol",
    "io",
    "ssc_generated",
)


def layer_of(path: Path):
    rel = path.relative_to(SRC)
    if len(rel.parts) == 1:
        # 应用入口/组合根/调度器直接放在 src/ 根下。
        return "app" if rel.suffix in (".c", ".cpp", ".h", ".hpp") else None
    return rel.parts[0]


def check():
    problems = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        if "ssc_generated" in path.parts:
            continue  # licensed SSC sources are not our layer
        layer = layer_of(path)
        if layer not in LAYER_ALLOWED:
            continue
        allowed = LAYER_ALLOWED[layer]
        external = EXTERNAL_ALLOWED[layer]
        for number, line in enumerate(
            path.read_text(errors="ignore").splitlines(), 1
        ):
            match = INCLUDE.search(line)
            if not match:
                continue
            include = match.group(1)
            if include in SSC_NAMES:
                if layer != "adapters":
                    problems.append(
                        f"{path.relative_to(ROOT)}:{number}: SSC header "
                        f"{include} outside adapters"
                    )
                continue
            if include.startswith(THIRD_PARTY) or include in external:
                continue
            if include.startswith(allowed):
                continue
            if include.split("/")[0] in REPO_ROOTS:
                problems.append(
                    f"{path.relative_to(ROOT)}:{number}: {include} "
                    f"not allowed in {layer}/"
                )
    return problems


def main() -> int:
    problems = check()
    if problems:
        for problem in problems:
            print(f"FORBIDDEN: {problem}")
        return 1
    print("OK: common/domain/drivers/adapters/app layering is respected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
