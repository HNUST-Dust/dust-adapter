# MCU CAN device and protection configuration

`can_devices.yaml` is validated and compiled into the firmware. It is not a file
read by the MCU at runtime; FatFs is not required. Changing device topology or
protection bytes requires rebuilding/flashing the firmware.

```yaml
schema_version: 1
command_timeout_us: 20000       # Maximum host lease; host may select a shorter one.
protect_on_boot: true          # Default true; submit CAN protection on MCU startup.
can_buses:
  - channel: 3                 # Physical CAN controller index, 0–3.
    bitrate: 1000000
    devices:
      - name: example_joint   # Human-readable unique name; not a motor driver/model.
        frame_format: standard # Optional: standard (default) or extended.
        command_id: 0x003     # Allowed host TX address; null/omitted for RX-only devices.
        command_remote: false # Optional RTR flag for host TX.
        feedback_ids: [0x013] # Exact receive-ID filters, not a don't-care mask.
        feedback_remote: false
        protection_frame:
          id: 0x003          # Optional; defaults to command_id.
          remote: false
          data: [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd]
```

The declared `command_id` and any separate protection-frame ID form the transmit
allowlist. Payloads are still raw bytes: MCU does not decode MIT, motor faults,
positions or torque. Feedback filters match ID, standard/extended format and RTR.
Unknown feedback IDs are not forwarded; unknown TX addresses/flags are rejected
before any part of that packet is published.

Each TX device must explicitly declare `protection_frame`; `null` is allowed for a
device that needs no protection write. RX-only devices need feedback IDs and cannot
declare a protection write. A protection data list has 0–8 bytes (its length is DLC).
RTR data bytes carry no payload semantics. Bitrates are compile-time configuration;
the CAN driver must support the selected rate. UART parameters are unchanged and
remain fixed in MCU/devicetree configuration.

Shared command/group IDs are supported. Identical protection writes to the same
bus/ID/format/RTR are deduplicated; conflicting bytes/lengths fail the build.
Protection order follows the first occurrence in the YAML. The default file keeps
left W/B/D then right W/B/D, without hardcoding that policy in generic C++ logic.

Limits: four physical CAN channel indices, sixteen RX filters per configured bus
(HPM driver limit), 32 protection frames overall. The TX queue can hold 32 records per bus. Duplicate YAML keys,
unknown keys, invalid IDs/bytes/types, duplicate bus indices, conflicting protection
writes and limit overflow are build errors. Missing/disabled configured CAN hardware
fails initialization instead of silently dropping a declared device.

The default robot profile contains:

| Bus | Device | Command ID | Feedback ID | Protection payload |
|---|---|---|---|---|
| CAN3 | left wheel |0x200 |0x201 | eight zero bytes |
| CAN3 | left joint B |0x000 |0x010 | FF FF FF FF FF FF FF FD |
| CAN3 | left joint D |0x003 |0x013 | FF FF FF FF FF FF FF FD |
| CAN2 | right wheel |0x200 |0x201 | eight zero bytes |
| CAN2 | right joint B |0x001 |0x011 | FF FF FF FF FF FF FF FD |
| CAN2 | right joint D |0x002 |0x012 | FF FF FF FF FF FF FF FD |

Custom profile selection:

```bash
./dust-adapter/scripts/build.sh ethercat -- -DADAPTER_CAN_CONFIG=/absolute/path/can_devices.yaml
```

The firmware owns the CAN protection baseline and maximum command lease. The
EtherCAT configuration exposes its protection count and CRC32 signature so the
host can check that its configuration matches before arming. This signature is
configuration identity, not authentication or a complete feedback-routing hash.

Protection is an attempted transmission, not proof of delivery or physical stopping.
Already in-flight CAN frames cannot be recalled. Physical bus/electrical faults can
prevent delivery. Repeating protection indefinitely is not implemented: one set is
submitted on each trigger (and on startup when enabled).

空闲总线可声明 `devices: []`：初始化总线，但不添加发送白名单、接收转发过滤器或保护帧。

## 台架配置（config/labs/）

`config/labs/` 存放仅用于实验室台架的可复现 CAN 白名单 profile，不参与默认构建。
使用 `-DADAPTER_CAN_CONFIG=<绝对路径>` 指定任意 profile：

| 文件 | 用途 |
|---|---|
| `labs/can_bench_can3_peripheral.yaml` | CAN3 三地址失能/零力矩台架，排除 CAN2 |
| `labs/can_bench_dm_rtt.yaml` | 仅 CAN3 DM ID3 的 RTT 测量台架 |
| `labs/can_bench_rx_only.yaml` | 只接收不发送的台架首轮验证 |

默认构建仍使用 `config/can_devices.yaml`；台架 profile 不得合并进默认拓扑。

## 未焊接板载 IMU 的替换板

构建时追加 `-DEXTRA_CONF_FILE=config/no_onboard_imu.conf`。
该 profile 关闭板载 SPI 采集和 IMU 加热，并显式允许固定 DTG1 页保留但无传感器。
28 字节板载区域保持无效/零值，HI91、CAN 保护、布局签名与 20 ms 看门狗不变。
装回板载 IMU 后重新使用默认 `prj.conf` 构建，不要沿用此 profile。
