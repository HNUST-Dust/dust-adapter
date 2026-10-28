#!/usr/bin/env python3
"""Dump bounded trace using the matching ELF symbol. Run only after master exits."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--nm', required=True)
p.add_argument('--openocd', required=True)
p.add_argument('--scripts', required=True, help='hpm_sdk/boards/openocd')
a = p.parse_args()
entries = subprocess.check_output([a.nm, '-S', '--defined-only', str(a.elf)], text=True)
symbols = [line.split() for line in entries.splitlines() if line.split()[-1:] == ['dust_ecat_trace']]
if len(symbols) != 1:
    p.error('ELF has no unique dust_ecat_trace symbol; build with CONFIG_DUST_ECAT_RTT_TRACE=y')
address, size = (int(x, 16) for x in symbols[0][:2])
output = str(a.output.resolve())
if any(c in output for c in '{}\\\n\r'):
    p.error('output path contains unsupported Tcl characters')
a.output.parent.mkdir(parents=True, exist_ok=True)
# Always resume after a failed dump as well. No reset: that would erase the trace.
dump = f'set failed [catch {{dump_image {{{output}}} 0x{address:x} {size}}} detail]; resume; if {{$failed}} {{error $detail}}'
command = [a.openocd, '-s', a.scripts, '-f', 'probes/cmsis_dap.cfg', '-f', 'soc/hpm5e00.cfg',
           '-c', 'cmsis-dap vid_pid 0x1209 0x6666', '-c', 'cmsis-dap backend usb_bulk',
           '-c', 'reset_config trst_only', '-c', 'adapter speed 500', '-c', 'init',
           '-c', 'halt', '-c', dump, '-c', 'shutdown']
subprocess.run(command, check=True)
if a.output.stat().st_size != size:
    raise RuntimeError('dump size differs from ELF symbol size')
print(f'Trace exported: {output} ({size} bytes); CPU resumed')
