# ESC DMA experiment (HPM5E31)

`CONFIG_DUST_ECAT_ESC_DMA` is default **n**. The CPU word path remains the production default. This experiment uses Zephyr MEMORY_TO_MEMORY DMA, XDMA channel 6, 32-bit incrementing addresses, one-word bursts and a 1536-byte uncached bounce buffer. UART4..6 use XDMA 0..5. The application overlay reserves 6 and a compile-time scan rejects other enabled devicetree DMA users of the same controller/channel.

Only aligned PDO blocks starting exactly at the generated SM2/SM3 addresses use DMA. Registers, mailbox, unsupported sizes and interrupt/masked-interrupt contexts keep the CPU path before DMA starts. The PDI copy API stays synchronous: wait for the DMA IRQ callback, then copy received bytes into the SSC buffer or return from a completed write. This version busy-waits and does not claim to release CPU time through asynchronous pipelining.

The HPM DMA driver converts local RAM addresses into system bus addresses. The bounce buffer is placed with `__nocache`/32-byte alignment; normal SSC buffers are not accessed by DMA, so this implementation avoids adding cache maintenance or relying on their alignment. No PDO ABI or CAN command change is introduced.

Initialization saves an unused 512-byte PDRAM span at 0xfe00, writes a deterministic pattern with DMA, checks it with CPU reads, reads it with DMA and checks every byte, then restores the original span. The gateway starts only if both directions pass. `DUST_RAM_END <= 0xfe00` is asserted. On a failed test the scratch span may remain modified, but it is outside all allocated SM partitions.

A configuration/start/completion error or 500 us timeout latches a transport error, requests DMA stop, and prevents further DMA transfers. There is **no CPU retry after DMA starts**. Failed read buffers are zeroed; the gateway protects before processing another command and does not publish a possibly partial transfer. IRQ-context accesses never wait on the DMA IRQ. Reboot is required to clear a latched fault.

Build the lab disable-only image:

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_dma -- \
 -DDUST_ADAPTER_CAN_CONFIG="$PWD/config/can_bench_dm_rtt.yaml" \
 -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_DUST_ECAT_ESC_DMA=y \
 -DCONFIG_BUILD_OUTPUT_BIN=y
bash scripts/test_ethercat_esc_dma.sh
```

The native test compiles the actual backend with only peripheral/kernel boundaries replaced. It covers complete staging and exact payloads, PDRAM self-test restoration/corruption, configuration/start/completion failures, timeout, sticky failure, and CPU fallbacks without starting DMA. The actual SSC integration test injects a DMA fault while Active and checks protection runs once and command dispatch stops. The default disabled target also builds and the full protocol/IO regression passes.

After the bounded master exits, export both symbols with the matching ELF:

```bash
python3 tools/dump_ethercat_rtt_trace.py ELF trace.bin --nm NM --openocd OPENOCD --scripts SDK_OPENOCD
python3 tools/dump_ethercat_rtt_trace.py ELF dma-stats.bin --symbol dust_esc_dma_stats \
 --nm NM --openocd OPENOCD --scripts SDK_OPENOCD
```

Stats ABI v1 is 64 bytes `<12Ii3I`: version, ready, selftest, reads, writes, cpu_fallbacks, irq_fallbacks, config_errors, start_errors, completion_errors, timeouts, reentries, last_error, channel, burst, max_wait_cycles. Counts include all eligible PDO transfers, not just the 64 motor commands. `cpu_fallbacks` also includes routine control/register accesses; it is not a count of failed PDO DMA operations. max_wait_cycles uses the 24 MHz hardware clock and excludes configuration/pre-copy time.

## 2026-10-28 result

125 us FreeRun, Rx96/Tx400, FIFO40 CPU7, temporary ASPM-off then restored; 64 sequential CAN3 ID3 disable frames. All commands, trace joins and sequence checks passed; no WKC miss, timeout, DMA error or IRQ fallback. Selftest=1, reads=6754, writes=6756, XDMA channel6/burst1. DMA trace image RAM 227276 B (86.70%), ROM 171500 B.

| Mean time (us) | DMA | Adjacent CPU word control |
|---|---:|---:|
| ESC read 96B | 29.935 | 13.915 |
| ESC write 400B | 71.775 | 49.296 |
| CAN submit to RX callback | 6648.271 | 275.212 |
| NUC RTT | 7223.552 | 770.455 |

The CPU control used the same previously verified word-fast image. Its maximum RTT was 998.982 us; DMA maximum was 7748.980 us. All 16 trace points were complete in both runs. No hard real-time conclusion follows from 64 samples.

This **synchronous bounce-buffer/IRQ implementation is slower**. The much larger change in CAN submit-to-RX-callback occurred only in the DMA run, and disappeared in the adjacent CPU control. It is not explained by the roughly 38.5 us additional mean PDO copying cost. The measurements do not distinguish physical CAN/motor reply delay from RX IRQ latency; the mechanism remains unresolved. Do not label the extra 6 ms as Ethernet transfer time or proven motor computation time.

The CPU image was restored to hardware. DMA remains an opt-in experiment. Larger bursts, a safe polled DMA driver API or a different asynchronous architecture would require separate investigation and measurements; the existence of SYNC0/1 DMA request signals alone does not establish a performance benefit. Raw host/MCU data and build fingerprints are saved in dust-control/docs/bench-results with the 2026-10-28 dma and cpucontrol prefixes.

## Corrected MCAN receive path: multithread + DMA retest

Subsequent MCAN hardware diagnostics found an eight-frame FIFO backlog with
6.512 ms SOF age and only 5.935 us ISR-entry-to-callback time. The combined
hardware mask/global rejection/FIFO drain correction removed that anomaly in
the multithread CPU run: FIFO occupancy 1, hardware age 122 us including frame
reception, RTT mean 872.004 us/max 999.708 us, all 64 commands/trace records pass.
This evidence changes the interpretation of the earlier unexplained CAN segment;
it does not retrospectively reconstruct the exact backlog in the older DMA run.

The new DMA comparison keeps `CONFIG_DUST_ECAT_CAN_RX_THREAD=y`,
`CONFIG_HPM_MCAN_RX_TIMING=y`, the corrected hpm_support revision and all
125 us/64 sample bench parameters. Build directory is `build/dm_rtt_mcan_fixed_dma`;
DMA stays opt-in. Export DTR1, MCT1 and `dust_esc_dma_stats` after the master exits.
Do not compare only host RTT: also compare ESC read/write, hardware FIFO age,
CAN callbacks, publish wait and successful DMA transfer/error counts.
New combined-image runtime performance is pending.

### Corrected-path result (64 samples)

The combined multithread/DMA test passed 64 commands with complete DTR1/MCT1
records, no WKC miss or timeout, and restored ASPM. FIFO occupancy stayed 1;
hardware SOF age averaged 124.250 us (maximum 144 us). CAN submit-to-RX-callback
was 301.103 us versus CPU 300.981 us: the old 6 ms anomaly did not recur.

| Metric, us unless indicated | Multithread CPU | Multithread DMA |
|---|---:|---:|
| RTT mean | 872.004 | 961.917 |
| RTT P95 | 999.127 | 1124.164 |
| RTT maximum | 999.708 | 1249.018 |
| RTT standard deviation | 84.187 | 114.148 |
| Samples over 1 ms / 64 | 0 | 15 |
| ESC read96B mean | 16.334 | 41.005 |
| ESC write400B mean | 54.468 | 93.961 |
| Snapshot to publish mean | 59.974 | 94.978 |

Actual DMA reads=4822/writes=4855, ready=selftest=1, channel6/burst1; all error,
timeout, reentry and IRQ-fallback counts zero. CPU fallbacks include register
accesses and are not DMA failure counts. RAM diagnostics and build fingerprints
are saved under dust-control/docs/bench-results with prefix
2026-10-28-dm-rtt-125us-mcanfixdma.

This synchronous bounce-buffer/configure/wait-for-IRQ implementation remains
slower in this small-block experiment even after the CAN issue was fixed.
It does not overlap transfer completion with useful SSC work. It is not evidence
that every DMA architecture or burst setting is slower. Keep the multithread
architecture and CPU word default; the tested CPU image is restored after the
comparison. A future DMA optimization needs separate timing and correctness
validation. Sixty-four samples do not establish long-term latency bounds.
