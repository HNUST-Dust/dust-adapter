#!/usr/bin/env python3
"""Validate MCU CAN devices and generate fixed filters, TX addresses and protection frames."""
import argparse
from pathlib import Path
import struct
import zlib
import yaml


class UniqueKeyLoader(yaml.SafeLoader):
    """Reject duplicate YAML keys instead of silently replacing a safety setting."""


def unique_mapping(loader, node, deep=False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if not isinstance(key, str) or key in mapping:
            raise ValueError(f'duplicate or non-string configuration key: {key!r}')
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, unique_mapping)


def load_config(text):
    return yaml.load(text, Loader=UniqueKeyLoader)


def integer(value, low, high, label):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{label}: expected integer {low}..{high}')
    return value


def keys(obj, allowed, label):
    if not isinstance(obj, dict) or obj.keys() - set(allowed):
        raise ValueError(f'{label}: expected mapping with keys {allowed}')


def flag(obj, name):
    value = obj.get(name, False)
    if type(value) is not bool:
        raise ValueError(f'{name}: expected boolean')
    return value


def compile_profile(source):
    keys(source, ('schema_version', 'command_timeout_us', 'protect_on_boot', 'can_buses'), 'profile')
    if type(source.get('schema_version')) is not int or source.get('schema_version') != 1:
        raise ValueError('schema_version must be 1')
    timeout = integer(source.get('command_timeout_us'), 1000, 1000000, 'command_timeout_us')
    boot = flag(source, 'protect_on_boot') if 'protect_on_boot' in source else True
    buses, filters, addresses, stops = [], [], [], []
    names, channels = set(), set()
    stop_by_address = {}
    entries = source.get('can_buses')
    if not isinstance(entries, list) or not entries:
        raise ValueError('can_buses must be a nonempty list')
    for bus in entries:
        keys(bus, ('channel', 'bitrate', 'devices'), 'bus')
        channel = integer(bus.get('channel'), 0, 3, 'channel')
        if channel in channels:
            raise ValueError('duplicate CAN channel')
        channels.add(channel)
        bitrate = integer(bus.get('bitrate'), 10000, 1000000, 'bitrate')
        buses.append((channel, bitrate))
        devices = bus.get('devices')
        if not isinstance(devices, list):
            raise ValueError('each bus must declare a devices list')
        for dev in devices:
            keys(dev, ('name', 'frame_format', 'command_id', 'command_remote',
                       'feedback_ids', 'feedback_remote', 'protection_frame'), 'device')
            name = dev.get('name')
            if not isinstance(name, str) or not name or name in names:
                raise ValueError('device names must be nonempty and unique')
            names.add(name)
            fmt = dev.get('frame_format', 'standard')
            if fmt not in ('standard', 'extended'):
                raise ValueError('frame_format must be standard or extended')
            extended = int(fmt == 'extended')
            maximum = 0x1fffffff if extended else 0x7ff
            command_flags = extended | (int(flag(dev, 'command_remote')) << 1)
            feedback_flags = extended | (int(flag(dev, 'feedback_remote')) << 1)
            feedback = dev.get('feedback_ids', [])
            if not isinstance(feedback, list):
                raise ValueError('feedback_ids must be a list')
            for value in feedback:
                entry = (channel, integer(value, 0, maximum, 'feedback_id'), feedback_flags)
                if entry not in filters:
                    filters.append(entry)
            command = dev.get('command_id')
            if command is None:
                if not feedback:
                    raise ValueError('device needs command_id or feedback_ids')
                if dev.get('protection_frame') is not None:
                    raise ValueError('RX-only device cannot declare protection_frame')
                continue
            command = integer(command, 0, maximum, 'command_id')
            address = (channel, command, command_flags)
            if address not in addresses:
                addresses.append(address)
            # Explicit null is permitted for devices that do not need a protection write.
            if 'protection_frame' not in dev:
                raise ValueError('TX device must explicitly declare protection_frame (or null)')
            protection = dev['protection_frame']
            if protection is None:
                continue
            keys(protection, ('id', 'remote', 'data'), 'protection_frame')
            stop_id = integer(protection.get('id', command), 0, maximum, 'protection id')
            stop_flags = extended | (int(flag(protection, 'remote')) << 1)
            data = protection.get('data')
            if not isinstance(data, list) or len(data) > 8:
                raise ValueError('protection data must be a list of 0..8 bytes')
            data = [integer(value, 0, 255, 'protection byte') for value in data]
            stop_address = (channel, stop_id, stop_flags)
            record = (channel, len(data), stop_flags, stop_id, data + [0] * (8 - len(data)))
            if stop_address in stop_by_address and stop_by_address[stop_address] != record:
                raise ValueError('shared command/protection ID has conflicting protection payloads')
            if stop_address not in stop_by_address:
                stops.append(record)
                stop_by_address[stop_address] = record
            if stop_address not in addresses:
                addresses.append(stop_address)
        # The pinned HPM hardware/driver has sixteen receive filters per bus.
        if sum(item[0] == channel for item in filters) > 16:
            raise ValueError('more than 16 receive filters on one CAN bus')
    if len(stops) > 32:
        raise ValueError('at most 32 firmware protection frames are supported')
    packed = struct.pack('<I', timeout)
    for channel, length, flags, can_id, data in stops:
        packed += struct.pack('<BBBBI8B', channel, length, flags, 0, can_id, *data)
    return dict(timeout=timeout, boot=boot, buses=buses, filters=filters,
                addresses=addresses, stops=stops, signature=zlib.crc32(packed))


def header(profile):
    def array(name, typ, entries):
        return (f'inline constexpr std::array<{typ}, {len(entries)}> {name} = {{{{\n' +
                ''.join('    ' + entry + ',\n' for entry in entries) + '}};\n')
    result = '''/* SPDX-License-Identifier: Apache-2.0 */
// Generated from config/can_devices.yaml; do not edit.
#pragma once
#include <array>
#include <cstdint>
namespace dust::can_profile {
struct CanFrame { uint8_t channel; uint8_t length; uint8_t flags; uint32_t id; uint8_t data[8]; };
struct Bus { uint8_t channel; uint32_t bitrate; };
struct Address { uint8_t channel; uint32_t id; uint8_t flags; };
'''
    result += f'inline constexpr uint32_t kCommandTimeoutUs = {profile["timeout"]}U;\n'
    result += f'inline constexpr bool kProtectOnBoot = {str(profile["boot"]).lower()};\n'
    result += f'inline constexpr uint32_t kProtectionSignature = 0x{profile["signature"]:08x}U;\n'
    result += array('kBuses', 'Bus', [f'{{{ch}, {rate}U}}' for ch, rate in profile['buses']])
    for name, key in [('kReceiveFilters', 'filters'), ('kTransmitAddresses', 'addresses')]:
        result += array(name, 'Address', [f'{{{ch}, 0x{can_id:x}U, {flags}}}' for ch, can_id, flags in profile[key]])
    result += array('kProtectionFrames', 'CanFrame', [
        f'{{{ch}, {length}, {flags}, 0x{can_id:x}U, {{{", ".join(hex(v) for v in data)}}}}}'
        for ch, length, flags, can_id, data in profile['stops']])
    return result + '} // namespace dust::can_profile\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        profile = compile_profile(load_config(args.input.read_text()))
    except (ValueError, yaml.YAMLError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header(profile))


if __name__ == '__main__':
    main()
