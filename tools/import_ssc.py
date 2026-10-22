#!/usr/bin/env python3
"""Import locally licensed, already configured SSC sources; never downloads SSC."""
import argparse
import hashlib
import json
import pathlib
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=pathlib.Path, help='Configured ssc_generated/src directory')
parser.add_argument('--destination', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1]/'external/ssc')
args = parser.parse_args()
source = args.source.resolve()
required = ('ecat_def.h', 'ecatslv.c', 'ecatappl.c', 'sampleappl.c', 'sampleappl.h', 'applInterface.h', 'eeprom.h')
for name in required:
    if not (source/name).is_file():
        parser.error(f'Missing {name}; supply the configured SSC source directory')
config = (source/'ecat_def.h').read_text()
if 'hpm5e_ssc' not in config:
    parser.error('SSC configuration does not reference the HPM5E hardware binding')
if args.destination.exists():
    parser.error('Destination exists; choose a new directory rather than overwriting licensed files')
args.destination.parent.mkdir(parents=True, exist_ok=True)
shutil.copytree(source, args.destination)
hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
          for p in sorted(args.destination.iterdir()) if p.is_file()}
(args.destination/'import_hashes.json').write_text(json.dumps(hashes, indent=2)+'\n')
print(f'Imported {len(hashes)} configured SSC files to {args.destination}; existing licence headers retained.')
