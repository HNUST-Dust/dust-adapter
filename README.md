# dust-adapter — generic USB adapter (protocol v2)

`feature/generic-usb-adapter` implements the generic adapter. The released `v0.1`
uses the older robot-specific v1 protocol. **The two protocols are not compatible.**
Use matching `dust-control` branch `feature/generic-usb-adapter`; neither side
silently falls back to the old protocol.

The dust-hpm6750 firmware forwards Classical CAN frames, fixed-configuration UART
byte streams and the board-mounted raw IMU. MCU device addresses, RX filters and
default protection writes now come from [config/can_devices.yaml](config/can_devices.yaml).
It contains no actuator-slot logic, SBUS/HI91 parser, MIT decoder or chassis algorithm.
The generic driver does not interpret device payloads; it enforces the configured
addresses and applies raw protection bytes independently of the host.
The `ethercat` branch remains the independent slave foundation.

## Build

Python3/West/PyYAML, CMake/Ninja and a compatible Zephyr SDK are required.

```bash
mkdir dust-workspace && cd dust-workspace
west init -m https://github.com/HNUST-Dust/dust-adapter.git --mr feature/generic-usb-adapter
west update
west zephyr-export
./dust-adapter/scripts/build.sh usb
```

Dependencies are pinned in `west.yml`, including the UART partial-publication and
CAN RTR software-filter fixes. No manual patch is needed. No FatFs dependency.
The SDK defaults to `sdk_env/hpm_sdk`; `HPM_SDK_DIR` can override that path.

## Board interfaces

| Logical interface | Hardware | Fixed configuration |
|---|---|---|
| CAN channels | Enabled devicetree CAN nodes, numbered 0–3 | Profile-configured bitrate and exact ID/format/RTR filters |
| UART port 1 | UART10 | 100000 baud, 8E2; 128-byte RX double buffer, 1000 us line-idle publication |
| UART port 2 | UART2 | 921600 baud, 8N1; circular DMA with existing outstanding-write guard |
| UART port 3 | UART3 | 115200 baud, 8N1; async RX double buffer |
| Board IMU | Existing SPI2 sensor | Existing raw burst, timestamp and temperature acquisition |

UART settings are MCU compile-time/devicetree configuration. There is no USB
command to change baud rate, parity or stop bits. UART writes use async DMA and
bounded per-port FIFO queues. Board wiring still determines which TX/RX pins
are physically accessible; logical support does not add pins to the board.

## Session and safety

`Hello → Ready → ConfigureSafety → Result → Arm → Result → data/heartbeat`.
Opening a session enables reception only. The MCU owns a persistent CAN protection
baseline compiled from its device configuration. The host acknowledges/selects this
baseline and a lease no longer than the MCU maximum, then explicitly Arms.
Zero records in ConfigureSafety mean "use MCU defaults", never "remove protection".

On expiry, USB reset/disconnect, explicit Stop or detected output failure, the MCU
invalidates the session, discards queued business writes and submits its configured
CAN protection frames. They are not erased on session close. Startup also submits
them by default. Optional host-registered UART stop actions remain per-session.
Frames already in CAN hardware cannot be recalled; bus faults can prevent protection
delivery. These actions do not prove physical shutdown. See [configuration reference](config/README.md).

See [wire protocol](docs/usb.md), [migration and commissioning](docs/GENERIC_USB_MIGRATION.md).
Packet builders are in `host/include/dust_adapter/client.hpp`, and the canonical
wire header is `include/dust_adapter/protocol.hpp`.

## Verification and CI

```bash
./scripts/test_protocol.sh
```

The native tests check wire bounds, flag validation, session gating, replay,
lease expiry and timestamp/sequence wrap under AddressSanitizer/UBSan.
GitHub Actions builds the firmware and stores BIN/ELF, configuration, dependency
manifest and checksums. Existing v0.1 is unchanged; this branch is not automatically
flashed or promoted to a production release. Compilation and mocked USB regressions
are not hardware acceptance or a CAN/device RTT measurement.
