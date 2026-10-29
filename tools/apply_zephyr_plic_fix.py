#!/usr/bin/env python3
"""Apply ordered PLIC backports after validating in a temporary source tree."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--zephyr',type=Path,default=root.parent/'zephyr')
a=p.parse_args()
patches=sorted((root/'patches/zephyr').glob('000*.patch'))
relative=Path('drivers/interrupt_controller/intc_plic.c')
source=a.zephyr/relative
original=source.read_bytes()
def apply(directory,patch,reverse=False):
    command=['git','apply',*(['--reverse'] if reverse else []),str(patch)]
    return subprocess.run(command,cwd=directory,capture_output=True).returncode==0
# Test every already-applied prefix; reverse later patches before checking earlier
# ones so adjacent hunk contexts do not break idempotence. No real-file writes yet.
for prefix in range(len(patches),-1,-1):
    with tempfile.TemporaryDirectory() as tmp:
        target=Path(tmp)/relative;target.parent.mkdir(parents=True);target.write_bytes(original)
        if not all(apply(tmp,patch,True) for patch in reversed(patches[:prefix])):
            continue
        if not all(apply(tmp,patch) for patch in patches):
            continue
        result=target.read_bytes()
    if source.read_bytes()!=original:
        p.error('PLIC source changed during validation; no files modified')
    if result==original:
        print('All PLIC backports already applied')
    else:
        source.write_bytes(result)
        print('PLIC backports applied after temporary-tree validation')
    break
else:
    p.error('PLIC source does not match any known patch state; no files modified')
