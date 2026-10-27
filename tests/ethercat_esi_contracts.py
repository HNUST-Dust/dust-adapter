#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""同源 ESI / SII / SSC 上限与空默认 PDO 的离线契约（不替代 ETG 一致性测试）。"""
import importlib.util
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
module_spec = importlib.util.spec_from_file_location("generator", ROOT / "tools/generate_gateway_esi.py")
generator = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(generator)
spec = json.loads((ROOT / "apps/ethercat_slave/gateway.json").read_text())
config, header, xml, image = generator.generate(spec)
assert xml == (ROOT / "apps/ethercat_slave/esi/DUST_HPM5E31_Gateway.xml").read_text()
assert len(image) == 2048 and image[14] == 0x30
assert struct.unpack_from("<III", image, 16) == (spec["vendor_id"], spec["product_code"], spec["revision"])
assert spec["vendor_id"] == 0 and spec["product_code"] != 0x57425231
assert spec["rx_address"] + 3 * spec["rx_max"] <= spec["tx_address"]
assert spec["tx_address"] + 3 * spec["tx_max"] <= spec["ram_end"]
assert "#define MAX_PD_OUTPUT_SIZE 0x400U" in config
assert "#define MAX_PD_INPUT_SIZE 0x600U" in config
assert len(header.split("0x")) == 2049
categories = {}
offset = 128
while True:
    kind, words = struct.unpack_from("<HH", image, offset)
    offset += 4
    if kind == 0xffff:
        break
    assert kind not in categories and offset + words * 2 <= len(image)
    categories[kind] = image[offset:offset + words * 2]
    offset += words * 2
assert 50 not in categories and 51 not in categories  # 不暴露旧计数器映射。
assert categories[30][5] == 0x0f
assert categories[40] == bytes([1, 2, 3, 255])
root = ET.fromstring(xml)
device = root.find("./Descriptions/Devices/Device")
assert device is not None
identity = device.find("Type")
assert int(identity.attrib["ProductCode"].replace("#x", "0x"), 16) == spec["product_code"]
assert device.find("RxPdo") is None and device.find("TxPdo") is None
coe = device.find("Mailbox/CoE")
assert coe.attrib["PdoAssign"] == coe.attrib["PdoConfig"] == "true"
assert coe.attrib["CompleteAccess"] == "false"
for n, sm in enumerate(device.findall("Sm")):
    address, size, control, status, enabled, kind = struct.unpack_from("<HHBBBB", categories[41], n * 8)
    assert int(sm.attrib["StartAddress"].replace("#x", "0x"), 16) == address
    assert int(sm.attrib["DefaultSize"]) == size
    assert int(sm.attrib["ControlByte"].replace("#x", "0x"), 16) == control
    assert enabled == 1 and kind == n + 1
assert len(device.findall("Sm")) == 4
print("ESI / SII / SSC identity, SM partitions and dynamic CoE contracts PASS")

for changes in ({"tx_address": 0x1400}, {"rx_address": 0x1101},
                {"rx_max": 1023}, {"rx_max": 0}, {"tx_max": 4097}):
    try:
        generator.generate({**spec, **changes})
    except ValueError:
        continue
    raise AssertionError(f"Accepted invalid partitions: {changes}")
