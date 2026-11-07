# Multirate publication bench

This opt-in experiment separates CAN/status preparation from UART/IMU preparation.
It does not split EtherCAT frames or reduce PDO bytes. Enable
`config/labs/multirate.conf`; retain the 125 us host cycle and single SSC owner.
Every new output image still drives command validation, dispatch and fast status
publication. State transitions force a full publication. While Active, full
UART/IMU publication is scheduled at least 1000 us after the previous full
publication (with loop quantization, not a guaranteed exact 1 kHz).

Fast images preserve the previous UART/IMU bytes and do not advance UART windows.
CAN and diagnostics continue updating. Stop/Protected force the full encoder so
retained sensor validity cannot survive protection. Main-loop sensor acquisition,
UART service and watchdog checks continue at their normal cadence. This is not
an independent IMU thread and does not decimate sensor acquisition itself.

Build firmware with the lab config plus `ECAT_BENCH_TIMING=y` and
`ECAT_TX_LATENCY_BENCH=y` for comparison only. Use the same UART0 reservation
overlay as the CPU/DMA comparison for both measured builds; DMA stays off.
历史独立探针（正式化时撤下，源码保存在验收归档） records unique CAN age,
status RTT, IMU validity/sequence consistency, and raw UART chunks/discontinuities.
Raw UART continuity does not establish HI91 CRC correctness or wire-level loss.
IMU updates count publication sequence changes, not physical DRDY samples.

Run the native codec contract from the workspace root after generating the PDO
header with a firmware build:

```sh
c++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -DCONFIG_ECAT_MULTIRATE_PUBLISH=1 \
  -DCONFIG_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR=1 \
  -Idust-adapter/src -Idust-adapter/build/multirate/generated \
  dust-adapter/tests/probes/multirate/codec_contract.cpp \
  dust-adapter/src/domain/protocol/{session,codec,wire,can_channel,uart_channel}.cpp \
  dust-adapter/src/domain/pdo/layout.cpp -o /tmp/dust-multirate-codec-test
/tmp/dust-multirate-codec-test
```

The contract checks byte preservation, independently advancing diagnostics and
publication sequence, and clearing UART/IMU validity on protection. All hardware
runs register no motor command producers. Restore the normal image after testing.
