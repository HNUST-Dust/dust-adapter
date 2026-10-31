#!/usr/bin/env python3
"""Generate the shared PDO header from pdo_schema.yaml.

Output is a single C++ header (drop-in for both dust-adapter and dust-control)
containing:

  1. the ordered field walk (WalkRx / WalkTx) that emits every
     (object, name, bits) tuple in layout order, including byte-array chunking
     and dynamic slot/byte counts from the configuration;
  2. compile-time named offsets for the runtime encode/decode code, so no
     hand-written layout builder or magic offset is needed.

The output is deterministic: regenerating from the same schema yields a
byte-identical header, and the walk reproduces the ABI 5.0 layout exactly.
"""
import argparse
from pathlib import Path

import yaml


def chunks_of(total_bytes, chunk):
    """Yield (byte_offset, byte_length) for a <=30-byte chunked array."""
    for start in range(0, total_bytes, chunk):
        yield start, min(chunk, total_bytes - start)


class Field:
    def __init__(self, name, bits, is_bytes, byte_count):
        self.name = name
        self.bits = bits
        self.is_bytes = is_bytes
        self.byte_count = byte_count


class Region:
    def __init__(self, spec, direction):
        self.direction = direction
        self.name = spec["name"]
        self.object = spec["object"]
        self.count = spec.get("count", 1)
        self.prefix = spec.get("prefix", [self.name])
        self.slots_from = spec.get("slots_from")
        self.bytes_from = spec.get("bytes_from")
        self.if_ = spec.get("if")
        self.header = [self._field(k, v) for k, v in spec.get("header", {}).items()]
        self.frame = [self._field(k, v) for k, v in spec.get("frame", {}).items()]
        self.fixed = [self._field(k, v) for k, v in spec.get("fields", {}).items()]

    @staticmethod
    def _field(name, value):
        if isinstance(value, int):
            return Field(name, value, False, 0)
        return Field(name, None, True, value["bytes"])

    @property
    def is_indexed(self):
        return self.slots_from is not None

    @property
    def is_sized(self):
        return self.bytes_from is not None


def load(schema_path):
    doc = yaml.safe_load(Path(schema_path).read_text())
    rx = [Region(r, "rx") for r in doc["rx"]]
    tx = [Region(r, "tx") for r in doc["tx"]]
    return doc, rx, tx


def header_size(region):
    return sum(f.bits // 8 for f in region.header) if region.header else 0


def frame_size(region):
    return sum(f.bits // 8 if not f.is_bytes else f.byte_count for f in region.frame)


def fixed_size(region):
    total = 0
    for f in region.fixed:
        total += f.byte_count if f.is_bytes else f.bits // 8
    return total


def region_size(region):
    if region.is_indexed:
        return header_size(region) + frame_size(region)
    if region.is_sized:
        return header_size(region)
    return fixed_size(region)


def c_name(direction, region, field):
    """C++ identifier for a field offset constant."""
    return field.name


def emit_offsets(out, direction, regions):
    ns = "rx" if direction == "rx" else "tx"
    out.append(f"namespace {ns} {{")
    for region in regions:
        out.append(f"namespace {region.name} {{")
        if region.is_indexed:
            out.append(f"inline constexpr size_t header_size = {header_size(region)};")
            out.append(f"inline constexpr size_t frame_stride = {frame_size(region)};")
            for f in region.header:
                off = sum(p.bits // 8 for p in region.header[:region.header.index(f)])
                out.append(f"inline constexpr size_t {f.name} = {off};")
            for f in region.frame:
                off = sum((p.byte_count if p.is_bytes else p.bits // 8)
                          for p in region.frame[:region.frame.index(f)])
                out.append(f"inline constexpr size_t frame_{f.name} = {off};")
        elif region.is_sized:
            out.append(f"inline constexpr size_t header_size = {header_size(region)};")
            for f in region.header:
                off = sum(p.bits // 8 for p in region.header[:region.header.index(f)])
                out.append(f"inline constexpr size_t {f.name} = {off};")
        else:
            out.append(f"inline constexpr size_t size = {fixed_size(region)};")
            off = 0
            for f in region.fixed:
                out.append(f"inline constexpr size_t {f.name} = {off};")
                off += f.byte_count if f.is_bytes else f.bits // 8
        out.append("}  // namespace " + region.name)
    out.append("}  // namespace " + ns)


def emit_walk(out, direction, regions):
    fn = "WalkRx" if direction == "rx" else "WalkTx"
    slots = "can_tx" if direction == "rx" else "can_rx"
    out.append(f"// {direction.upper()} field walk, in layout order. `emit(object, name, bits)`")
    out.append("template <typename F>")
    out.append(f"void {fn}(const Config& c, F&& emit) {{")
    out.append("    char name[96];")
    for region in regions:
        if region.is_indexed:
            for i in range(region.count):
                obj = f"0x{region.object + i:04x}"
                pfx = region.prefix[i]
                out.append(f"    if (c.{slots}[{i}]) {{")
                for f in region.header:
                    out.append(f'        emit({obj}, "{pfx}.{f.name}", {f.bits});')
                out.append(f"        for (unsigned j = 0; j < c.{slots}[{i}]; ++j) {{")
                for f in region.frame:
                    if f.is_bytes:
                        for start, length in chunks_of(f.byte_count, 30):
                            out.append(f'            std::snprintf(name, sizeof(name), "{pfx}.frames[%u].{f.name}[{start}]", j);')
                            out.append(f'            emit({obj}, name, {length * 8});')
                    else:
                        out.append(f'            std::snprintf(name, sizeof(name), "{pfx}.frames[%u].{f.name}", j);')
                        out.append(f'            emit({obj}, name, {f.bits});')
                out.append("        }")
                out.append("    }")
        elif region.is_sized:
            src = region.bytes_from
            for i in range(region.count):
                obj = f"0x{region.object + i:04x}"
                pfx = region.prefix[i]
                out.append(f"    if (c.uart_tx[{i}] || c.uart_rx[{i}]) {{")
                for f in region.header:
                    out.append(f'        emit({obj}, "{pfx}.{f.name}", {f.bits});')
                out.append(f"        for (unsigned n = 0; n < c.{src}[{i}]; n += 30) {{")
                out.append(f'            std::snprintf(name, sizeof(name), "{pfx}.data[%u]", n);')
                out.append(f'            emit({obj}, name, (c.{src}[{i}] - n < 30 ? c.{src}[{i}] - n : 30) * 8);')
                out.append("        }")
                out.append("    }")
        else:
            obj = f"0x{region.object:04x}"
            pfx = region.prefix[0]
            if region.if_:
                out.append(f"    if (c.{region.if_}) {{")
            for f in region.fixed:
                if f.is_bytes:
                    for start, length in chunks_of(f.byte_count, 30):
                        out.append(f'        emit({obj}, "{pfx}.{f.name}[{start}]", {length * 8});')
                else:
                    out.append(f'        emit({obj}, "{pfx}.{f.name}", {f.bits});')
            if region.if_:
                out.append("    }")
    out.append("}")
    out.append("")


def emit_production(out, doc):
    """Emit the fixed production profile (Config + cycle) from the schema."""
    if "production" not in doc:
        return
    p = doc["production"]
    c_tx = ", ".join(str(int(x)) for x in p["can_tx"])
    c_rx = ", ".join(str(int(x)) for x in p["can_rx"])
    u_tx = ", ".join(str(int(x)) for x in p["uart_tx"])
    u_rx = ", ".join(str(int(x)) for x in p["uart_rx"])
    imu = "true" if p.get("imu") else "false"
    out.append("// Fixed production wiring (single source of truth).")
    out.append(f"inline constexpr uint32_t kProductionCycleUs = {int(p['cycle_us'])};")
    out.append("inline constexpr Config ProductionConfig() {")
    out.append(f"    return {{{{{c_tx}}}, {{{c_rx}}}, {{{u_tx}}}, {{{u_rx}}}, {imu}}};")
    out.append("}")


def generate(schema_path, output_path):
    doc, rx, tx = load(schema_path)
    major = doc["abi"]["major"]
    minor = doc["abi"]["minor"]
    lines = []
    lines.append("/* SPDX-License-Identifier: Apache-2.0 */")
    lines.append(f"// GENERATED from {Path(schema_path).name} — DO NOT EDIT.")
    lines.append("// Single source of truth for the EtherCAT process image layout.")
    lines.append("#pragma once")
    lines.append("#include <array>")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("#include <cstdio>")
    lines.append("")
    lines.append("namespace dust::pdo {")
    lines.append(f"inline constexpr uint16_t kAbiMajor = {major};")
    lines.append(f"inline constexpr uint16_t kAbiMinor = {minor};")
    lines.append("")
    lines.append("// Runtime configuration used by the field walk.")
    lines.append("struct Config {")
    lines.append("    std::array<uint8_t, 2> can_tx{};   // Rx: chassis/gimbal command slots")
    lines.append("    std::array<uint8_t, 4> can_rx{};   // Tx: per-bus feedback slots")
    lines.append("    std::array<uint16_t, 6> uart_tx{}; // Rx: host->slave bytes per port")
    lines.append("    std::array<uint16_t, 6> uart_rx{}; // Tx: slave->host bytes per port")
    lines.append("    bool imu = false;")
    lines.append("};")
    lines.append("")
    emit_production(lines, doc)
    lines.append("")
    lines.append("// ---- compile-time offsets for runtime encode/decode ----")
    emit_offsets(lines, "rx", rx)
    lines.append("")
    emit_offsets(lines, "tx", tx)
    lines.append("")
    lines.append("// ---- ordered field walks (layout + CRC + CoE mapping) ----")
    emit_walk(lines, "rx", rx)
    emit_walk(lines, "tx", tx)
    lines.append("}  // namespace dust::pdo")
    lines.append("")
    output_path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.schema, args.output)


if __name__ == "__main__":
    main()
