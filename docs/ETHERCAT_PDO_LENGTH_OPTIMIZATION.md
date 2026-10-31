# EtherCAT PDO 长度优化方案（评审稿）

状态：仅评审，未改代码。数据基于 `src/ethercat/protocol.hpp`、`src/ethercat/layout.cpp`、
`src/ethercat/core.cpp` 与 `docs/ETHERCAT_PDO_PROTOCOL.md` 的 ABI 5.0 定义。

## 1. 背景与目标

当前 125 μs 周期下，紧凑/Orb 默认布局为 **RxPDO 172 B / TxPDO 672 B**（签名 `0xb6e99713`），
净传输 844 B/周期。本方案目标：在不改变控制/安全语义的前提下压缩 PDO 长度，降低总线占用与
ESC 搬运成本；所有字段增删都会改变布局签名，必须主从同源生成器同步、重新算 CRC 并重跑离线回归。

## 2. 当前字段级明细（ABI 4.0）

区域按 `layout.cpp` 中的生成顺序排列，偏移为紧凑布局下的实际位置。

### 2.1 RxPDO = 172 B（主站 → adapter）

| 对象 | 偏移 | 字段 | 字节 | 小计 | 实际使用 |
|---|---|---|---|---|---|
| 0x2000 | 0 | session_id (u64) | 8 | 28 | 会话 |
| | 8 | expected_boot_id (u64) | 8 | | 身份 |
| | 16 | exchange_sequence (u32) | 4 | | 去重/半空间 |
| | 20 | control_flags | 1 | | Discover/Arm/Stop |
| | 21 | active_source_mask | 1 | | 来源掩码 |
| | 22 | status_target | 1 | | 状态查询 |
| | 23 | status_channel | 1 | | 状态查询 |
| | 24 | status_request_sequence (u32) | 4 | | 状态查询 |
| 0x2010 | 28 | source_sequence (u32) | 4 | 16 头 | 命令序号 |
| | 32 | remaining_valid_us (u32) | 4 | | 期限 |
| | 36 | flags (u16) | 2 | | 仅 0/1 |
| | 38 | frame_count | 1 | | 帧数 |
| | 39 | source_id | 1 | | 来源 |
| | 40 | reserved (u32) | 4 | | **强制为 0** |
| | 44 | 帧 0..5 × 16 | 96 | 6 帧 | 见下 |
| 每帧 | | channel 1 + length 1 + flags 1(**必须0**) + reserved 1(**必须0**) + id 4 + data 8 | 16 | | id 仅 11 bit |
| 0x2021 | 140 | UART2 反向头（tx=0 B） | 16 | 16 | 仅 peer_ack 4 B 有效 |
| 0x2024 | 156 | UART5 反向头（tx=0 B） | 16 | 16 | 同上 |

### 2.2 TxPDO = 672 B（adapter → 主站）

| 对象 | 偏移 | 字段 | 字节 | 小计 | 实际使用 |
|---|---|---|---|---|---|
| 0x3000 | 0 | boot_id (u64) | 8 | 32 | 身份 |
| | 8 | accepted_session_id (u64) | 8 | | 会话 |
| | 16 | publish_sequence (u32) | 4 | | 发布序号 |
| | 20 | slave_now_us (u32) | 4 | | 从站时钟低 32 位 |
| | 24 | capability_mask (u32) | 4 | | 仅数位有效 |
| | 28 | business_state | 1 | | 状态 |
| | 29 | fault_flags | 1 | | 故障 |
| | 30 | reserved (u16) | 2 | | **未使用** |
| 0x3012 | 32 | batch_sequence (u32) | 4 | 88 | 反馈序号 |
| | 36 | cumulative_drops (u32) | 4 | | 累计丢弃 |
| | 40 | frame_count (u16) | 2 | | 帧数 |
| | 42 | flags (u16) | 2 | | **未使用** |
| | 44 | reserved (u32) | 4 | | **未使用** |
| | 48 | 帧 0..2 × 24 | 72 | | 见下 |
| 0x3013 | 120 | 同 0x3012（CAN3） | 88 | 88 | |
| 每帧 | | id 4 + bus_sequence 4 + capture_us 4 + length 1 + flags 1(**未使用**) + reserved 2(**未使用**) + data 8 | 24 | | id 仅 11 bit |
| 0x3021 | 208 | UART2：block_seq 4 + peer_ack 4 + capture 4 + length 2 + flags 2 + data 128 | 144 | 144 | |
| 0x3024 | 352 | UART5：同上 + data 32 | 48 | 48 | |
| 0x3030 | 400 | sample_sequence 4 + capture_us 4 + flags 4(**恒为1**) + temperature 4 + spi 25 + reserved 7(**未使用**) | 48 | 48 | |
| 0x3040 | 448 | 诊断区 | 224 | 224 | 见 §2.3 |

### 2.3 诊断区 0x3040 = 224 B（TxPDO 的 33%）

| 偏移 | 字段 | 字节 |
|---|---|---|
| 448 | stop_reason | 4 |
| 452 | rejected | 4 |
| 456 | can_drops | 4 |
| 460 | uart_drops | 4 |
| 464 | protection_errors | 4 |
| 468 | configuration_errors | 4 |
| 472 | firmware_flags | 4 |
| 476 | status_reply_sequence | 4 |
| 480..608 | 4 总线 × (received/submitted/completed/dropped/errors/queue_depth/controller_state/packed_error_counts) × 4 | 128 |
| 608..656 | 6 端口 × (rx_drops/tx_errors) × 4 | 48 |
| 656 | status_result | 4 |
| 660 | status_value0 | 4 |
| 664 | status_value1 | 4 |
| 668 | status_value2 | 4 |

诊断区每个 125 μs 周期完整发出一次，即约 8 kHz × 224 B ≈ 1.8 MB/s 的连续遥测，且绝大部分
（128 B CAN 计数 + 48 B UART 计数）是调试/监控用途，不是控制或安全数据。

## 3. 长度构成与瓶颈

TxPDO 672 B：诊断 33% > UART 29% > CAN 反馈 26% > IMU 7% > 头 5%。
RxPDO 172 B：CAN 命令 65% + 头 16% + 两个 UART 反向空头 19%。

结论：**TxPDO 的 224 B 诊断区是唯一的大头**，其次是跨两方向的保留/未使用字节与 CAN 帧元数据冗余。

## 4. 优化机会清单

按语义风险分三档。收益为单方向、紧凑布局下的估算，实际值需用生成器重新计算。

### 4.1 零语义（字段当前从未写入/恒为 0，行为不变，仍属 ABI 布局变更）

| 项 | 位置 | 现状 | 收益(B) |
|---|---|---|---|
| Tx CAN 反馈每帧 flags+reserved | 0x3012/0x3013 每帧 | 3 B 从不写 | 6 帧 × 3 = **18** |
| Tx CAN 反馈每通道 flags+reserved | 0x3012/0x3013 头 | 6 B 从不写 | 2 × 6 = **12** |
| Tx 头 reserved | 0x3000+30 | 2 B 从不写 | **2** |
| IMU flags+reserved | 0x3030+8 | flags 恒 1、reserved 7 B 从不写 | **10** |
| Rx CAN 命令每帧 flags+reserved | 0x2010 每帧 | 强制为 0 | 6 × 2 = **12** |
| Rx CAN 命令每源 reserved | 0x2010+40 | 强制为 0 | **4** |

小计：Tx **−42 B**，Rx **−16 B**（合计 −58 B，约 −7%）。

### 4.2 低语义（结构瘦身，行为等价，需小幅验证）

| 项 | 位置 | 方案 | 收益(B) |
|---|---|---|---|
| 诊断区移出周期 | 0x3040 | 保留 stop_reason/rejected/status 回复(4)+result/value(4×4) 约 32 B，其余 CAN/UART 计数走 CoE SDO 按需读 | Tx **−192** |
| 诊断计数降位宽 | 0x3040 | drops/errors 改 u16/u8（若保留在周期内） | 视保留项，Tx −64 ~ −128 |
| UART 反向空头 | 0x2021/0x2024 | 只收端口仅保留 4 B peer_ack + 2 B flags（≈6 B） | Rx **−20** |
| CAN 反馈 id 位宽 | 每帧 | u32 → u16（标准帧 11 bit） | Tx 6 × 2 = **−12** |
| CAN 命令 id 位宽 | 每帧 | u32 → u16 | Rx 6 × 2 = **−12** |

小计（诊断移出 + id 位宽 + 反向空头）：Tx **−204**，Rx **−32**。

### 4.3 语义权衡（涉及身份/时序/兼容，需单独评审）

| 项 | 现状 | 方案 | 收益 | 权衡 |
|---|---|---|---|---|
| boot/session ID | 各 64 bit | 降 48 或 32 bit | 每 ID −4/−8 B，两方向合计 −16~−32 B | 身份冲突概率上升；契约文档明确“原有 64 位未缩短” |
| capture_us / slave_now_us | u32 µs，回绕约 71.6 min | 降位宽或改时基 | 每字段 −2~−3 B | 回绕策略/时间同步需重评 |
| exchange/publish 序号 | u32 | u16 | 每字段 −2 B | 半空间回绕在高频下更易误判 |
| capability_mask | u32 仅数位有效 | u16 | Tx −2 B | 能力位预算收紧 |

## 5. 组合方案估算（紧凑布局基线 172/672）

| 方案 | 内容 | 估算结果 | 降幅 |
|---|---|---|---|
| A 最小改动 | 仅 §4.1 | Rx 156 / Tx 630 | −7% |
| B 平衡 | §4.1 + 诊断移出周期 + id 位宽 + 反向空头 | **Rx 140 / Tx 468** | **−19%** |
| C 激进 | B + 64 位 ID 缩短 + 时间戳/序号位宽 | Rx ~120 / Tx ~400 以下 | −30% 以上 |

B 方案不触碰身份/时序语义，收益最大且风险可控，建议作为首选评审对象。

## 6. 影响面（任何一项都必须一并处理）

- **布局签名**：`BuildLayout()` 的 CRC32 覆盖 ABI/周期/完整映射与字段名，任何字段变更都会改变
  `0xb6e99713`，主站 `pdo_config.cpp` 必须同源同步，否则 `Commit()` 报 `Signature/Mapping`。
- **同源生成器**：从站 `layout.cpp` 与主站 `pdo_config.cpp`、`generate_gateway_esi.py` 生成的
  ESI/SII/SSC 上限必须一致；缩字段要同步改 `docs/ETHERCAT_PDO_PROTOCOL.md` 与 `config/pdo_schema.yaml`。
- **核心/后端**：`Feedback` 结构（`std::array<uint8_t,224> diagnostics`）、`Publish()`/`Process()`
  的偏移硬编码（如 `diagnostics + 28/+208`）、io_backend 的统计提供方都要同步改。
- **测试点**：`tests/ethercat_compact_layout.cpp`（172/672 断言）、`ethercat_layout_crosscheck.cpp`、
  `ethercat_core_contracts.cpp`（`diagnostic+208`）、`ethercat_io_contracts.cpp`、`ethercat_mapping_contracts.cpp`
  中的固定尺寸/偏移断言需一并更新。
- **兼容基线**：`REFACTOR_CLEANUP_HANDOFF_2026-10-30.md` 明确警告“重构时不要顺手缩减 PDO 改变比较条件”，
  且当前烧录固件为 ABI 2 的 208/688；本优化需作为显式 ABI 演进单独立项，不得混入无关清理。

## 7. 建议实施顺序

1. **冻结基线**：确认 `build/bench-host/selected-version-2026-10-30/` 与外部归档仍在，留回滚对照。
2. **先落 §4.1（零语义）**：同步主从、重算签名、改固定尺寸断言，跑 `scripts/test_ethercat_protocol.sh`
   与 `DUST_HOST_ROOT=... scripts/test_protocol.sh` 离线交叉回归。
3. **再落 §4.2 的诊断区移出周期**：明确 SDO 按需读接口与状态查询复用，评估 `status_request_sequence`
   现有机制是否可承载，避免破坏 `CompleteStatus()` 回显路径。
4. **§4.3 单独评审**：身份/时间戳/序号位宽涉及安全与回绕语义，需独立 RFC，不与前两档捆绑。
5. 每档通过离线回归并重测 125 μs WKC / 周期耗时后，再进入实机验收；禁止以网络线速估算代替 MCU/ESC 实测。

## 8. 本方案不涉及

- 不切换 DC / SM 同步、不改固定相位偏移、不重排字段顺序（保留比较条件）。
- 不删除 CAN 白名单、保护帧、Stop/期限/WKC 判据；不触碰 64 位身份 ID（除非 §4.3 单独立项）。
- 不把“窗口实验固件与正式主站不兼容”的遗留问题混入本次长度优化。

## 9. 方案 C 落地规范（ABI 5.0，已定稿）

状态：已选方案 C，按本节字段宽度同步修改主从。`kAbiMajor=5 / kAbiMinor=0`。

### 9.1 关键决策与取舍

| 决策 | 选择 | 理由/权衡 |
|---|---|---|
| boot/session ID | u64 → **u32** | 单总线 ≤3 adapter，32 位熵足够；牺牲身份冲突余量（契约曾选 64 位） |
| exchange/publish 序号 | **保持 u32** | publish 每周期 +1，8 kHz 下 u32 回绕约 6 天、u16 仅 8 s，缩短会误判乱序 |
| slave_now_us / capture_us | **保持 u32** | 时基回绕 71.6 min 已是文档化取舍，再缩位宽破坏年龄计算 |
| capability_mask | **保持 u32** | 能力位用到 bit31(保护)与 bits16–19(CAN 通道)，u16 需重排位且涉及 Arm 安全握手，2 字节不值得 |
| status_request_sequence | u32 → **u16** | 按需查询，低速率 |
| CAN 帧 id | u32 → **u16** | 标准帧 11 bit（校验 ≤0x7ff） |
| 诊断区 | 224 B → **28 B**，移出每周期 | 保留 stop_reason/rejected/status 回显；CAN/UART 计数遥测下掉（可后续走 SDO） |
| UART flags / CAN flags | u16 → **u8** | 仅 0/1 或少量位 |
| 各类 reserved/未用 flags | **删除** | 从不写或强制为 0 |

### 9.2 RxPDO（主站 → adapter）

- 公共头 0x2000 = **18 B**：session_id 4 + expected_boot_id 4 + exchange_sequence 4 + control_flags 1 + active_source_mask 1 + status_target 1 + status_channel 1 + status_request_sequence 2。
- CAN 源 0x2010/0x2011 头 = **11 B**：source_sequence 4 + remaining_valid_us 4 + flags 1 + frame_count 1 + source_id 1。每帧 = **12 B**：channel 1 + length 1 + id 2 + data 8。
- UART 0x202x 头 = **15 B**（双向对称）：block_sequence 4 + peer_ack_sequence 4 + capture_time_us 4 + length 2 + flags 1；数据按 tx/rx 容量。

### 9.3 TxPDO（adapter → 主站）

- 公共头 0x3000 = **22 B**：boot_id 4 + accepted_session_id 4 + publish_sequence 4 + slave_now_us 4 + capability_mask 4 + business_state 1 + fault_flags 1。
- CAN 反馈 0x3010–0x3013 头 = **9 B**：batch_sequence 4 + cumulative_drops 4 + frame_count 1。每帧 = **19 B**：id 2 + bus_sequence 4 + capture_time_us 4 + length 1 + data 8。
- UART 0x302x 头 = **15 B**（同上）。
- IMU 0x3030 = **38 B**：sample_sequence 4 + capture_time_us 4 + flags 1 + temperature_c 4 + spi_bytes 25。
- 诊断 0x3040 = **28 B**：stop_reason 4 + rejected 4 + status_reply_sequence 4 + status_result 4 + status_value0/1/2 各 4。

### 9.4 目标尺寸（紧凑布局：CAN2/3 各 3 反馈、chassis 6 命令、UART2 rx128、UART5 rx32、IMU 开）

- RxPDO = 18 + (11 + 6×12) + 15 + 15 = **131 B**（原 172）
- TxPDO = 22 + 2×(9 + 3×19) + (15+128) + (15+32) + 38 + 28 = **410 B**（原 672）
- 合计 844 B → **541 B**（**−36%**）。CAN3-only（无 CAN2）TxPDO ≈ 344 B。
- 布局签名（紧凑布局）`0xf7babb63`。

> 注：上述尺寸为规格推导，最终以主从两个生成器重新计算的 `layout_signature` 与字节数为准。
