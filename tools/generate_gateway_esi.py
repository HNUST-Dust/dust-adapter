#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""生成同源的 SSC 编译约束、RAM SII 镜像和动态 PDO 实验设备 ESI。"""
import argparse
import json
import struct
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def generate(spec, dc_sync=False):
    rx, tx = spec["rx_address"], spec["tx_address"]
    if not (0x1100 <= rx < tx < spec["ram_end"] <= 0x10000
            and rx % 4 == tx % 4 == 0
            and 0 < spec["rx_max"] <= 4096 and spec["rx_max"] % 2 == 0
            and 0 < spec["tx_max"] <= 4096 and spec["tx_max"] % 2 == 0
            and rx + 3 * spec["rx_max"] <= tx
            and tx + 3 * spec["tx_max"] <= spec["ram_end"]):
        raise ValueError("Invalid SM capacities, alignment or triple-buffer partitions")
    image = bytearray(2048)
    # SII 配置区 CRC-8：前 14 字节，初值 ff，多项式 07。
    crc = 0xff
    for value in image[:14]:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ (7 if crc & 0x80 else 0)) & 0xff
    image[14] = crc
    struct.pack_into("<IIII", image, 16, spec["vendor_id"], spec["product_code"], spec["revision"], 0)
    struct.pack_into("<HHHHH", image, 48, 0x1000, 128, 0x1080, 128, 4)
    struct.pack_into("<HH", image, 124, 15, 1)
    categories = bytearray()

    def category(kind, data):
        if len(data) % 2:
            data += b"\0"
        categories.extend(struct.pack("<HH", kind, len(data) // 2) + data)

    strings = [b"DUST Experimental Slaves", spec["name"].encode("ascii")]
    category(10, bytes([len(strings)]) + b"".join(bytes([len(s)]) + s for s in strings))
    general = bytearray(32)
    general[:6] = bytes([1, 0, 2, 2, 0, 0x0f])  # SDO/Info/Assign/Config，禁用 CA。
    general[11] = 4
    general[14] = 1
    general[16] = 0x11
    category(30, general)
    category(40, bytes([1, 2, 3, 0xff]))
    sm = [(0x1000, 128, 0x26, 1), (0x1080, 128, 0x22, 2),
          (rx, 0, 0x64, 3), (tx, 0, 0x20, 4)]
    category(41, b"".join(struct.pack("<HHBBBB", addr, size, control, 0, 1, kind)
                            for addr, size, control, kind in sm))
    categories.extend(b"\xff\xff\0\0")
    image[128:128 + len(categories)] = categories
    config = "/* SPDX-License-Identifier: Apache-2.0 */\n#pragma once\n"
    defines = {
        "VENDOR_ID": spec["vendor_id"], "PRODUCT_CODE": spec["product_code"],
        "REVISION_NUMBER": spec["revision"], "MAX_PD_OUTPUT_SIZE": spec["rx_max"],
        "MAX_PD_INPUT_SIZE": spec["tx_max"], "MIN_PD_WRITE_ADDRESS": rx,
        "MAX_PD_WRITE_ADDRESS": spec["ram_end"] - 1, "MIN_PD_READ_ADDRESS": tx,
        "MAX_PD_READ_ADDRESS": spec["ram_end"] - 1,
        "DUST_RX_ADDRESS": rx, "DUST_TX_ADDRESS": tx, "DUST_RAM_END": spec["ram_end"],
        "MIN_PD_CYCLE_TIME": 500000 if dc_sync else 1000000,
    }
    config += "".join(f"#define {name} 0x{value:x}U\n" for name, value in defines.items())
    config += f'#define DEVICE_NAME "{spec["name"]}"\n#define DEVICE_NAME_LEN {len(spec["name"])}\n'
    config += '#define DEVICE_HW_VERSION "dust-hpm5e31"\n#define DEVICE_HW_VERSION_LEN 12\n'
    config += '#define DEVICE_SW_VERSION "DTC1-lab"\n#define DEVICE_SW_VERSION_LEN 8\n'
    eeprom = "/* SPDX-License-Identifier: Apache-2.0 */\nunsigned char aEepromData[2048] = {\n"
    eeprom += "".join("    " + ",".join(f"0x{b:02x}" for b in image[i:i + 16]) + ",\n" for i in range(0, len(image), 16))
    eeprom += "};\n"
    root = ET.Element("EtherCATInfo", Version="1.6")
    vendor = ET.SubElement(root, "Vendor")
    ET.SubElement(vendor, "Id").text = f'#x{spec["vendor_id"]:08x}'
    ET.SubElement(vendor, "Name").text = "DUST Experimental Slaves"
    descriptions = ET.SubElement(root, "Descriptions")
    group = ET.SubElement(ET.SubElement(descriptions, "Groups"), "Group")
    ET.SubElement(group, "Type").text = "DUST_GATEWAY_LAB"
    ET.SubElement(group, "Name", LcId="1033").text = "DUST Experimental Slaves"
    device = ET.SubElement(ET.SubElement(descriptions, "Devices"), "Device", Physics="YY")
    ET.SubElement(device, "Type", ProductCode=f'#x{spec["product_code"]:08x}', RevisionNo=f'#x{spec["revision"]:08x}').text = "DUST_HPM5E31_GATEWAY_LAB"
    ET.SubElement(device, "Name", LcId="1033").text = spec["name"]
    ET.SubElement(device, "GroupType").text = "DUST_GATEWAY_LAB"
    for text in ("Outputs", "Inputs", "MBoxState"):
        ET.SubElement(device, "Fmmu").text = text
    for (addr, size, control, kind), name in zip(sm, ("MBoxOut", "MBoxIn", "Outputs", "Inputs")):
        attrs = dict(DefaultSize=str(size), StartAddress=f"#x{addr:04x}", ControlByte=f"#x{control:02x}", Enable="1")
        if kind < 3:
            attrs.update(MinSize="36", MaxSize="128")
        else:
            attrs.update(MinSize="0", MaxSize=str(spec["rx_max"] if kind == 3 else spec["tx_max"]))
        ET.SubElement(device, "Sm", **attrs).text = name
    mailbox = ET.SubElement(device, "Mailbox", DataLinkLayer="true")
    ET.SubElement(mailbox, "CoE", SdoInfo="true", PdoAssign="true", PdoConfig="true", CompleteAccess="false", SegmentedSdo="true")
    if dc_sync:
        dc = ET.SubElement(device, "Dc")
        mode = ET.SubElement(dc, "OpMode")
        ET.SubElement(mode, "Name").text = "DC-Sync0"
        ET.SubElement(mode, "Desc").text = "DC Sync0 owner-thread lab"
        ET.SubElement(mode, "AssignActivate").text = "#x0300"
        ET.SubElement(mode, "CycleTimeSync0", Factor="1").text = "0"
    ep = ET.SubElement(device, "Eeprom")
    ET.SubElement(ep, "ByteSize").text = "2048"
    ET.SubElement(ep, "ConfigData").text = image[:14].hex()
    ET.indent(root)
    xml = '<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n"
    return config, eeprom, xml, bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dc-sync0", action="store_true")
    args = parser.parse_args()
    spec = json.loads((ROOT / "config/ethercat_gateway.json").read_text())
    config, eeprom, xml, image = generate(spec, dc_sync=args.dc_sync0)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, text in (("gateway_config.h", config), ("eeprom.h", eeprom), ("DUST_HPM5E31_Gateway.xml", xml)):
        (args.output / name).write_text(text)
    (args.output / "gateway.sii").write_bytes(image)


if __name__ == "__main__":
    main()
