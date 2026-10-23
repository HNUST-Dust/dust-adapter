# Generic USB wire protocol v4

All multibyte integers are little endian. All host OUT transfers are exactly 128
bytes; MCU IN transfers are 64 bytes except CAN reports (40–160 bytes), over HS Bulk interface 0, OUT 0x01,
IN 0x81 (endpoint MPS 512). These transfer bounds are independent of device
count; multiple transfers may be submitted without waiting for peripheral feedback.
There is no periodic successful-data ACK.

## Common header (16 bytes)

| Offset | Size | Meaning |
|---|---|---|
| 0 | 4 | Magic `0x31425257` |
| 4 | 4 | OUT request sequence; IN software submission timestamp, MCU us low32 |
| 8 | 1 | Version 4 |
| 9 | 1 | Kind; see `src/usb_protocol.hpp` |
| 10–11 | 2 | Kind-specific count/channel |
| 12 | 4 | Nonzero host-selected session nonce |

Old protocol versions are rejected. Session ID changes invalidate sensor state on
both sides. MCU disconnect/reset increments a transport epoch and purges pending
USB OUT input; the epoch is checked independently of USB configured state.

## CAN request record (16 bytes)

`channel:u8, DLC:u8, flags:u8, reserved_zero:u8, id:u32, data[8]`.
Flags: bit0 extended ID; bit1 RTR. Standard IDs are <=0x7ff, extended <=0x1fffffff,
DLC <=8. CAN FD is not supported. RTR DLC describes requested length, not meaningful
payload bytes. All record padding is deterministic, but unused payload bytes need
not have semantic value. Channel existence is checked against Ready's CAN mask.

CanTransmitRequest (kind 3): count at byte10, byte11 zero, bytes16–31 zero, 0–6 records
starting at32. All trailing bytes are zero. Empty Transmit is a heartbeat.
The entire request is validated before admission. Per-bus queues are bounded FIFO;
there is no latest-value overwrite or motor slot. Transmit address/format/RTR must
match the MCU configuration; all records are checked before publication. Receive
filters are exact matches from the same configuration. Same-bus FIFO
order is retained by software; hardware scheduling/arbitration remains applicable.
Queue overflow rejects admission and closes an armed session, replacing queued
business traffic with configured stop actions. Across buses, publication is not
an atomic on-wire transaction.

CanReceiveReport (kind4): count at10 (1–6); records start at16, stride24: the 16-byte
record above + RX software timestamp u32 + per-bus RX sequence u32. Sequence advances
before USB enqueue, allowing the host to detect queue drops. Timestamps describe
software RX observation, not motor sampling/execution. The actual CAN report length is 16 + 24 × count, without waiting to fill six
records. Other report types remain 64 bytes. CAN TX completion means
hardware batch completion, never motor acknowledgement.

## UART

UartTransmitRequest (kind5): port at10, length1–96 at11, bytes16–31 zero, bytes at32,
remaining bytes zero. Output is FIFO per port. Parameters cannot be set over USB.

UartReceiveReport (kind6): port at10, length1–40 at11; stream sequence at16,
software observation time at20, data at24. These are chunks of a continuous stream,
not device frames. Sequence discontinuity requires the host parser to reset its
partial frame. All enabled ports use 128-byte asynchronous DMA double buffers; UART10 uses
optional board line-idle detection. Physical timing remains a hardware acceptance item.

## Board IMU

ImuSampleReport (kind7): burst length25 at10, data-ready software time at16, 25 raw SPI bytes at20,
float32 temperature at48, stream sequence at52. The sensor-specific board driver
is intentionally retained. No attitude estimator or robot frame transform runs here.

## Control plane

- Hello (kind1): 128 bytes, count/reserved/payload zero. Replaces any old session,
  applying its configured stop behavior. Begins reception, not output.
- Ready (kind2): capabilities at16 (CAN=1/UART10=2/UART2=4/IMU=8/UART3=16/UART11=32/UART12=64/UART15=128),
  OUT size128 at20, maximum IN size160 at24, CAN availability mask at28,
  max TX records6 at32, compiled CAN protection count at33, firmware-protection
  capability bit0 at34, protection signature u32 at36 and maximum lease u32 at40.
  The compiled table can exceed the USB six-record limit; count0 selects it.
- ConfigureSafety (kind8): only while disarmed; lease_us u32 at16, range1000 to the
  MCU-configured maximum. Count0 selects the MCU's default CAN protection table.
  It never clears protection. Uploaded CAN protection records are rejected.
  Flags4 UART actions may supplement the table (port1–6, ID0, length1–8),
  subject to transfer capacity.
  Startup, disconnect and lease protection use the compiled CAN table independently
  of host registration. The config must be acknowledged before Arm.
- Arm (kind9): zero payload. Starts lease immediately; successful data/Heartbeat
  renew it only with a strictly newer sequence. Expired leases cannot be revived.
- Heartbeat (kind11): zero payload, no ACK. Does not enable an unarmed session.
- Stop (kind10): invalidate session and apply configured actions. No success ACK;
  the host stops submitting business traffic and closes/drains USB independently.
- Result (kind12, IN): request sequence at16, request kind20, armed21, signed errno
  at24. Config/Arm always reply; invalid or failed data submissions reply where
  possible. Acceptance is not TX completion or physical execution.
- Status (kind13): page at10, channel at11, zero request payload. Does not renew
  command lease. Reply uses same page/channel. Counters wrap modulo2^32.

Status page0: armed16/configured17/last stop reason18; u32 counters at20 onwards:
rejected requests, CAN uplink drops, peripheral uplink drops, stop enqueue failures,
reply drops, USB send failures, USB OUT queue drops, endpoint errors.
Protection signature is at52 and maximum lease at56.
Detected OUT queue loss or endpoint errors close an armed session; invalid traffic
cannot renew the lease.
Page1 CAN: u32 at16 onwards: received frames, submitted frames, completed batches,
errors, discarded/rejected frames, queued frames, controller state, TX/RX error counters.
Page2 UART: u32 at16 onwards: received chunks, dropped chunks, TX DMA completions,
TX errors. DMA completion is not UART wire completion; the next DMA write waits for
hardware transmitter empty to avoid truncating a FIFO tail.

The counter interface is operational error visibility, not the former profiling
protocol. Inspect per-channel errors after reconnect; submitting a stop action does
not prove delivery. A stop action cannot be sent to a nonfunctional physical bus.
