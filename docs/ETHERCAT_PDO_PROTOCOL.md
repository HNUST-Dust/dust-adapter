# DTC EtherCAT PDO 协议（正式版 · ABI 5.0）

状态：正式生产布线。字段小端，长度由启动时 SDO 0x2100 配置构造、提交时用布局 CRC32 校验，运行期不可改。RxPDO = 主站 → adapter，TxPDO = adapter → 主站。

唯一事实源：`config/pdo_schema.yaml`（含 `production` 生产 profile）。生成的 `src/ethercat/pdo_schema.hpp` 提供命名偏移常量与字段 Walk，固件与主站同源使用。本文件是字段级可读说明。

## 1. 生产布线

只使用 CAN2、CAN3、UART2、UART5 与 SPI 板载 IMU。CAN0/1、UART1/3/4/6 不映射。

### 1.1 电机编址（6 个电机）

| 电机 | 总线(channel) | 命令 ID | 反馈 ID |
|---|---|---|---|
| 右轮 | CAN2 (2) | 0x200 | 0x201 |
| 右关节 B | CAN2 (2) | 0x001 | 0x011 |
| 右关节 D | CAN2 (2) | 0x002 | 0x012 |
| 左轮 | CAN3 (3) | 0x200 | 0x201 |
| 左关节 B | CAN3 (3) | 0x000 | 0x010 |
| 左关节 D | CAN3 (3) | 0x003 | 0x013 |

轮子命令 `0x200` 在两条总线上共用，靠命令帧的 `channel` 字节（2=右、3=左）区分。因此命令方向 6 槽 = 恰好 6 个电机；反馈方向 CAN2/CAN3 各 3 个 ID（`0x201` 同样靠总线区分）。

### 1.2 其它通道

| 通道 | 用途 | 方向与容量 |
|---|---|---|
| UART2 | HI91 姿态 | adapter→主站 RX 128 B |
| UART5 | SBUS 遥控 | adapter→主站 RX 32 B |
| SPI 板载 IMU | ICM-42688P | adapter→主站，25 B 原始 + 温度 |

## 2. 生产 Profile 与布局总览

生产 profile（`ProductionConfig()` + `kProductionCycleUs = 125`）：

```
can_tx = [6, 0]              # chassis 6 命令槽，gimbal 0
can_rx = [0, 0, 3, 3]        # CAN2 3 反馈槽，CAN3 3 反馈槽
uart_tx = [0,0,0,0,0,0]      # 无主站→从站 UART
uart_rx = [0,128,0,0,32,0]   # UART2 128 B，UART5 32 B
imu = true
```

| 项 | 值 |
|---|---|
| ABI | 5.0 |
| 周期 | 125 µs |
| RxPDO | **131 B** |
| TxPDO | **410 B** |
| 布局签名 | **`0xf7babb63`** |

对象索引总表：

| object | 区域 | 方向 | 字节 |
|---|---|---|---|
| 0x2000 | Rx 公共头 | Rx | 18 |
| 0x2010 | CAN 命令(chassis) | Rx | 83 |
| 0x2021 / 0x2024 | UART2 / UART5 反向头 | Rx | 15 / 15 |
| 0x3000 | Tx 公共头 | Tx | 22 |
| 0x3012 / 0x3013 | CAN2 / CAN3 反馈 | Tx | 66 / 66 |
| 0x3021 / 0x3024 | UART2 / UART5 | Tx | 143 / 47 |
| 0x3030 | IMU | Tx | 38 |
| 0x3040 | 诊断 | Tx | 28 |

## 3. RxPDO（主站 → adapter，131 B）

### 3.1 公共头 0x2000（18 B）

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| session_id | 32 | 0 | 主站生成的会话标识（u32） |
| expected_boot_id | 32 | 4 | 期望的从站 boot_id（Discover 可为 0） |
| exchange_sequence | 32 | 8 | 命令批次序号，半空间规则判新旧 |
| control_flags | 8 | 12 | Discover=1 / Arm=2 / Stop=4 |
| active_source_mask | 8 | 13 | 激活的命令源掩码（chassis=bit0） |
| status_target | 8 | 14 | 状态查询目标编码 |
| status_channel | 8 | 15 | 状态查询通道 |
| status_request_sequence | 16 | 16 | 状态查询请求序号 |

### 3.2 CAN 命令 0x2010（chassis，83 B = 11 头 + 6×12 帧）

头（11 B）：

| 字段 | 位宽 | 偏移 |
|---|---|---|
| source_sequence | 32 | 0 |
| remaining_valid_us | 32 | 4 |
| flags | 8 | 8 |
| frame_count | 8 | 9 |
| source_id | 8 | 10 |

每帧（12 B）：

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| channel | 8 | 0 | 目标 CAN 总线（2/3） |
| length | 8 | 1 | DLC |
| id | 16 | 2 | 标准帧 11 bit ID |
| data | 64 | 4 | 8 字节载荷 |

### 3.3 UART 反向头 0x2021 / 0x2024（各 15 B，数据 0 B）

主站只确认（ACK），不发送数据。字段同 §4.3 的头，`length` 恒 0。

## 4. TxPDO（adapter → 主站，410 B）

### 4.1 公共头 0x3000（22 B）

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| boot_id | 32 | 0 | 从站启动标识（每次重启变化） |
| accepted_session_id | 32 | 4 | 回显主站 session_id |
| publish_sequence | 32 | 8 | 每次发布 +1 |
| slave_now_us | 32 | 12 | 从站单调时钟低 32 位 |
| capability_mask | 32 | 16 | 能力位（bit31=保护，bits16–19=CAN 通道） |
| business_state | 8 | 20 | Ready/Active/Protected |
| fault_flags | 8 | 21 | 故障码 |

### 4.2 CAN 反馈 0x3012 / 0x3013（各 66 B = 9 头 + 3×19 帧）

头（9 B）：

| 字段 | 位宽 | 偏移 |
|---|---|---|
| batch_sequence | 32 | 0 |
| cumulative_drops | 32 | 4 |
| frame_count | 8 | 8 |

每帧（19 B）：

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| id | 16 | 0 | 标准帧 11 bit ID |
| bus_sequence | 32 | 2 | 该 ID 的接收序号 |
| capture_time_us | 32 | 6 | 采样时间（低 32 位） |
| length | 8 | 10 | DLC |
| data | 64 | 11 | 8 字节载荷 |

反馈语义：每个启用总线按编译期过滤表以标准帧 ID 分配独立槽，排空队列时只替换同一 ID。每次发布把当前有效槽按配置顺序紧凑写入；`frame_count` 为有效帧数。主站以 `(bus, id)` 识别、按 ID 去重，序号回绕用半空间规则。20 ms 无更新的槽失效。逐命令应答不保证；角度解绕由主站保证相邻观测不歧义。

### 4.3 UART 0x3021 / 0x3024（头 15 B + 数据）

头（15 B）：

| 字段 | 位宽 | 偏移 |
|---|---|---|
| block_sequence | 32 | 0 |
| peer_ack_sequence | 32 | 4 |
| capture_time_us | 32 | 8 |
| length | 16 | 12 |
| flags | 8 | 14 |

数据：UART2 128 B（chunk 30/30/30/30/8），UART5 32 B（chunk 30/2）。

UART 语义：每路独立四块发送窗口，序号从 1 开始，累计 ACK，只确认已发送窗口成员；重复 ACK 不前移，未确认块按序循环发布。下行只交付期望的下一块；重复/缺口不交付、不推进 ACK。20 ms 未完成块交付/确认进入保护。

### 4.4 IMU 0x3030（38 B）

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| sample_sequence | 32 | 0 | 采样序号 |
| capture_time_us | 32 | 4 | 采样时间 |
| flags | 8 | 8 | bit0 有效 |
| temperature_c | 32 | 9 | binary32 温度 |
| spi_bytes | 200 | 13 | 25 B 原始 SPI |

IMU 保留最新采样，adapter 解码温度并转发原始 SPI，上位机解析 HXY 六轴。芯片身份、DMA、温控需实机验收。

### 4.5 诊断 0x3040（28 B）

| 字段 | 位宽 | 偏移 | 说明 |
|---|---|---|---|
| stop_reason | 32 | 0 | 停机原因（fault） |
| rejected | 32 | 4 | 拒绝计数 |
| status_reply_sequence | 32 | 8 | 状态查询回复序号 |
| status_result | 32 | 12 | 状态查询结果 |
| status_value0 | 32 | 16 | 状态值 0 |
| status_value1 | 32 | 20 | 状态值 1 |
| status_value2 | 32 | 24 | 状态值 2 |

每总线/每端口计数遥测已移出周期 PDO，经既有状态查询机制（`RequestStatus`）按需读取。

## 5. 校验与保护（不变）

布局 CRC32 覆盖 ABI 版本、周期与完整映射，提交配置时计算并拒绝不匹配签名。运行期保留启动标识、会话、序号、能力与保护检查；WKC、来源期限、本地 CAN 白名单、保护帧、Stop 判据不变。CRC32 是兼容检查，不是认证。
