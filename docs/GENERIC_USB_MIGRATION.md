# v1 robot gateway → v2 generic adapter

The firmware no longer contains six-motor configuration, actuator slot names,
feedback-ID routing, DM Exit constants, MIT encoding, W/B/D scheduling or robot
message/parameter generation. UART settings remain fixed to the board's current
100000 8E2 / 921600 8N1 / 115200 8N1 configuration.

The paired host is Orb Ubuntu `~/dust-control`, branch `feature/generic-usb-adapter`.
Its device application owns motor mapping, per-cycle deduplication, W/B/D record
ordering, sensor parsing and six raw stop actions (wheel zero current, joint Exit).
The existing remote/IMU freshness and explicit output-enable policy remains there.
Open posts four IN transfers before Hello, then verifies Ready, configures a20ms
lease with the robot's stop actions, waits for Result, explicitly arms and waits
for Result before returning to the control loop. Arming the adapter is permission
to forward bytes, not an instruction to enable a motor.

Do not mix v1 firmware/host with v2. Old frames are rejected by version; there is
no permissive compatibility fallback. Existing release v0.1 remains available.
Both feature branches must be reviewed and commissioned as a pair before promotion.

## Completed offline checks

- Firmware compile for dust-hpm6750 with pinned HPM/Zephyr dependencies.
- Protocol bounds, standard/extended and data/remote flags, stale sequence,
  expired lease, wraparound and explicit safety configuration native contracts.
- Paired host compile; USB mocked lifecycle/reconnect, rejected safety setup,
  existing chassis/sensor/configuration/telemetry regressions.

## Hardware acceptance still required

1. Use a stop-only host with matching firmware first; verify fixed UART stream
   continuity, timestamps, CAN ID forwarding and host sensor decoding.
2. Exercise non-motor CAN IDs, extended/RTR frames on a simulator/bench peer;
   check FIFO order, overflow counters, and whole-packet rejection.
3. Verify continuous UART TX with a loopback/peer, especially across DMA boundaries;
   verify UART10 TX pin availability before relying on it.
4. Configure known stop-only actions; terminate/freeze the host and disconnect USB
   while MCU remains independently powered. Check queue discard, lease behavior,
   session replacement and rearm gating. Measure physical TX/device stop separately.
5. Run combined CAN/UART/IMU load and measure end-to-end latency with the new FIFO
   and record format. Old p50/p99 results do not validate the rewritten driver.
6. Only after those checks, repeat the established remote enable/disable and real
   chassis tests with the operator's explicit enable-output flag.

No test/build script flashes hardware, enables an actuator or starts a real run.
