from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
t=(root/'src/ethercat/slave/esc_dma.c').read_text()
t=t[t.index('struct DmaStats {'):t.rindex('#endif')]
Path(sys.argv[1]).write_text(t)
