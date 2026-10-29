#!/usr/bin/env python3
"""Narrow optional adjustments to build copies; never rewrite imported SSC."""
import argparse
from pathlib import Path


def skip_unused_input_event(source):
    start = source.index('void PDO_InputMapping(void)')
    end = source.index('void PDO_OutputMapping(void)', start)
    function = source[start:end]
    lines = function.splitlines(keepends=True)
    reads = [i for i, line in enumerate(lines)
             if line.strip() == 'UINT16 ALEvent = HW_GetALEventRegister_Isr();']
    if len(reads) != 1:
        raise ValueError('Unsupported SSC input mapping: expected one event read')
    i = reads[0]
    if i + 1 >= len(lines) or lines[i + 1].strip() != 'ALEvent = SWAPWORD(ALEvent);':
        raise ValueError('Unsupported SSC event read/swap pair')
    remainder = ''.join(lines[:i] + lines[i + 2:])
    if 'ALEvent' in remainder:
        raise ValueError('SSC input mapping uses ALEvent; refusing to remove read')
    lines[i:i + 2] = ['    /* Build-only adjustment: omit unused AL event read; measurement unchanged. */\n']
    return source[:start] + ''.join(lines) + source[end:]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build_copy', type=Path)
    a = p.parse_args()
    with a.build_copy.open(newline='') as f:
        source = f.read()
    updated = skip_unused_input_event(source)
    with a.build_copy.open('w', newline='') as f:
        f.write(updated)
