# CAN receive sampling independent of RxPDO

The FreeRun thread now polls gateway IO immediately before and after SSC MainLoop. Active CAN receive queues are drained into retained snapshots independently of new host outputs. APPL_Application also samples CAN before command processing. Each pass drains at most 32 events per bus (the queue depth); newer events wait for the next pass rather than extending the loop without a bound. A snapshot update or expiry marks CAN feedback dirty.

In the Active FreeRun main thread, a dirty snapshot is published and submitted immediately through SSC PDO_InputMapping, with at most one extra input refresh per gateway poll. This uses the SSC buffer/mapping/write function and does not directly bypass its ESC access path. It is guarded by mapping_ready, input-running and !bEscIntEnabled; no ISR or synchronized/DC path is added.

A dirty CAN snapshot can trigger a publication even without a new RxPDO. Normal publications (new RxPDO, state transition, SAFEOP periodic publication) still consume UART input and rotate UART windows. Extra CAN-only publications bind the latest CAN snapshot and preserve the previous UART process-image regions byte-for-byte. They do not queue UART input, mark another block sent or advance the UART retransmission cursor. Core::Publish accepts an internal optional advance_uart argument, default true; false requires an existing image with the current magic/boot/session/layout. Protected-state publications still clear UART data normally.

The fieldbus PDO layout, YAML capacities, CAN allowlists, latest-snapshot retention/expiry and command watchdogs are unchanged. This does not make the slave application loop a fixed 8 kHz task: SSC processing, ESC accesses and the 10 us busy wait still contribute latency. DMA stays disabled by default.

## Timestamp ABI v2

DTR1 header remains 40 bytes; version is now 2. Record size increases from 168 to 184 bytes, capacity remains 128. The first 16 stage numbers are unchanged. Two timestamps are appended at indices 16 and 17:

- CAN_ENQUEUED: matching feedback successfully copied into the receive queue.
- CAN_DEQUEUED: matching feedback copied out of the receive queue.

They join using the CAN receive sequence already stored with CAN_RX. The existing CAPTURE timestamp now marks retained snapshot completion in the independent CAN service. Thus RX to ENQUEUED approximates callback enqueue work, ENQUEUED to DEQUEUED includes queue wait/get overhead, and DEQUEUED to CAPTURE includes remaining draining/validation/snapshot work. The timestamps themselves add small overhead. CPU and host clocks remain independent.

The host decoder accepts v1/168 and v2/184 simultaneously. It checks the valid bitmap according to the record version and outputs queue timing columns only for v2. Re-decoding the previous real v1 CPU-control dataset produces byte-identical CSV.

## Validation

Native IO contracts verify CAN can drain/update a snapshot without consuming queued UART bytes, retain fresh snapshots and expire stale ones. They verify repeated CAN-only publications preserve a UART block's complete process-image region and do not consume the next window. Actual SSC contracts verify a dirty CAN snapshot publishes without a new RxPDO or command dispatch, a main-thread poll triggers exactly one extra input mapping, and idle passes do not keep incrementing publication sequence. Trace contracts check sequence-matched queue stamps. Full protocol/IO/SSC/MCAN/UART/clock regression passes; host decoder tests cover both versions.

Build the receive-path bench with CPU word access and DMA off:

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_can_fast -- \
 -DDUST_ADAPTER_CAN_CONFIG="$PWD/config/can_bench_dm_rtt.yaml" \
 -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_DUST_ECAT_ESC_DMA=n \
 -DCONFIG_BUILD_OUTPUT_BIN=y
```

Use the existing 64-command disable-only RTT tool and export the matching ELF's dust_ecat_trace after the master exits. Follow the usual no-reset-before-export rule. This is a CAN bench; physical UART coexistence is not re-tested by that tool, although native UART window/state contracts cover the changed publication behavior.

## 2026-10-28 bench result

An intermediate capture-only change reduced callback-to-snapshot to 42.027 us but moved waiting into snapshot-to-publication (82.091 us). RTT mean was 797.787 us. This is why the final implementation includes the bounded immediate SSC input refresh instead of only moving the capture timestamp earlier.

Final 64-command run: all 18 points complete, sequence matched, no trace overflow, timeout or WKC failure. Mean callback-to-snapshot 63.288 us, decomposed into callback/enqueue 9.637 us, queue wait/get 52.148 us and post-dequeue snapshot work 1.503 us. Snapshot-to-publication 13.855 us. RTT mean 762.631 us, P95 874.005 us, maximum 999.137 us. Prior CPU control mean RTT was 770.455 us, callback-to-snapshot 85.365 us. These short runs do not establish a significant end-to-end improvement or a hard 1 ms bound. Extra trace points also add small measurement overhead.

Actual snapshot work is short; most measured callback-to-snapshot latency is waiting for the main thread. ESC write duration remains about 50 us, and the thread still must finish its current SSC/IO work before draining the queue. Existing timestamps do not assign all queue wait to individual thread activities. Raw host and MCU records for both stages are saved in dust-control under the 2026-10-28 canearly / canfast prefixes.

Final trace build: ROM 170860 B, RAM 229244 B (87.45%). Default non-trace build: ROM 169028 B, RAM 205580 B (78.42%). DMA remains off. The final image is left on the board; master test ended with Stop and temporary ASPM settings were restored.
