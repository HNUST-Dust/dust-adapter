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
./dust-adapter/scripts/build.sh
```

Dependencies are pinned in `west.yml`, including the UART partial-publication and
CAN RTR software-filter fixes. No manual patch is needed. No FatFs dependency.
The SDK defaults to `sdk_env/hpm_sdk`; `HPM_SDK_DIR` can override that path.

The repository root is the single Zephyr application: `CMakeLists.txt`,
`Kconfig`, `prj.conf`, `boards/` and `src/`. Platform drivers and their interfaces
are in `src/drivers/`; protocol and packet builders are in `src/`. USB receive frames
and their bounded queue are defined directly inside the USB driver; no message
schema or message-header generator is required. Build output defaults
to `build/firmware`; do not reuse caches configured with the former app path.

## Board interfaces

| Logical interface | Hardware | Fixed configuration |
|---|---|---|
| CAN channels | Enabled devicetree CAN nodes, numbered 0–3 | Profile-configured bitrate and exact ID/format/RTR filters |
| UART port 1 | UART10 | 100000 baud, 8E2; 128-byte RX double buffer, 1000 us line-idle publication |
| UART port 2 | UART2 | 921600 baud, 8N1; 128-byte asynchronous RX double buffer |
| UART port 3 | UART3 | 115200 baud, 8N1; async RX double buffer |
| Board IMU | Existing SPI2 sensor | Existing raw burst, timestamp and temperature acquisition |

All three ports use the same asynchronous RX callback and separate pairs of
128-byte noncached DMA buffers. UART2 no longer uses circular DMA or polling.
Board-specific partial-buffer publication is behind `src/board/uart_rx_support.hpp`.
The V3 DBUS input is inverted by Q2 and reaches UART10 RX/PC04 and the
PB13/TRGM1_P03 auxiliary input. The optional detector uses TRGM1/GPTMR2;
its source and overlay are selected by the dust-hpm6750 board configuration.
Other boards use the default support implementation and their native UART timeout.
UART10 TX is not exposed on the DBUS connector; software write support does not
imply a physical TX connection there.

UART2 and UART3 currently retain the HPM driver's native timeout behavior. The
1000 us API argument does not guarantee partial-buffer delivery at that interval
on this driver. Continuous RX, buffer turnover, and short-burst latency after this
change require hardware acceptance, particularly UART2 at 921600 baud.
After moving the overlay, rebuild an existing cache with `scripts/build.sh -p always`.

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
Packet builders are in `src/client.hpp`, and the canonical
wire header is `src/protocol.hpp`.

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
