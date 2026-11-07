# Compact UART PDO bench

Keep host125us, CPU ESC access, CAN and IMU on the ordinary publication path.
This is a lab experiment, not a production default. It does not use ESC DMA.

The existing ABI5 negotiates UART capacity and verifies the layout signature.
Changing host UART2 `rx_bytes` from128 to64 reduces TX410 to346 bytes; RX remains131.
HI91 is already decoded as a continuous byte stream and may span UART blocks.
No new page protocol, multiplexed CAN-only/IMU-only cycles or weakened WKC policy
is introduced. PDO length stays fixed for each configured session.

Cases:

| Case | MCU UART RX block | Host UART2 PDO capacity | Other change |
|---|---:|---:|---|
| baseline |128|128|none|
| PDO only |128|64|none|
| combined candidate |64|64|preserve IMU on late CAN refresh|

The combined firmware uses [uart64.conf](../../../config/labs/uart64.conf).
The existing 历史独立探针（正式化时撤下，源码保存在验收归档） takes `uart64`
as its second argument after duration. It derives IMU/diagnostics offsets from
the negotiated layout. The binary links existing dust_ethercat, dust_ethercat_pdo
and dust_imu libraries; include an RPATH for both hardware/ethercat and hardware/imu.
The PDO-only comparison uses the exact same MCU binary as baseline.
All measured firmware enables bench timing and commit timestamp marking, disables
UART0 through the same overlay and keeps ESC DMA/multirate publication off.

The host checks source mask/chassis frame count on Send; gimbal capacity is zero.
It records raw UART continuity, strict HI91 frame decode/CRC and monotonic device
time after stopping. Byte collection/CRC analysis is outside the RT master loop.
HI91 host reassembly wait is first received byte-chunk availability to final
byte-chunk availability. It includes main-consumer polling and is not synchronized
sensor-to-host latency. UART block age uses MCU event capture to ESC commit; it
excludes earlier serial accumulation. IMU sequence counts publications, not DRDY.
Censored WKC sessions are retained separately, not mixed into performance means.

The 64-byte DMA option also halves byte capacity of the four-entry hardware-facing
software RX queue (512 to256 bytes per active port), and roughly doubles HI91 block
traffic. No queue-depth increase is part of this experiment. Combined results
cannot isolate the cost of block frequency from the IMU-preservation change.

[Results and limitations](../../../docs/compact-pdo-2026-11-05.md).
