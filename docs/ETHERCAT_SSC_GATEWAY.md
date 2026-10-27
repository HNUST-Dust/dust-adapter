# HPM5E31 SSC 网关接入（实验阶段）

2026-10-27。当前构建已替换旧计数器应用，接入 `src/ethercat` 协议核心。
本次只做编译和原生离线测试，没有刷板；此前 1 kHz 旧计数器实测不作为新固件验收。

## 当前能力与边界

- SSC 对象：配置 `0x2100:1..22`（u32）；Rx 映射 `0x1600..0x1604`；
  Tx 映射 `0x1a00..0x1a04`；分配 `0x1c12/0x1c13`；业务对象在配置提交时按实际布局展开。
- PREOP 才可写，逐条 SDO 下载；不支持 Complete Access。参数与映射到达顺序任意。
  缺参数、CRC/布局不一致、超容量、不可用通道或不符实际 SM 配置，拒绝 SAFEOP。
- FreeRun，周期 1 ms；不支持 SM 同步/DC。SAFEOP 发布 Ready，OP 才处理新 Rx 影像。
  `APPL_OutputMapping` 捕获新包，`APPL_Application` 调用核心并编码 Tx，
  `APPL_InputMapping` 复制 Tx；主循环还独立检查 AL/硬件看门狗。
- CAN/UART 的实际驱动、输出白名单、本地停止回调、诊断分页尚未接入。
  不设置 capability bit31；Discover 可验证协议，但 Arm 返回 Protection 并锁存 Protected，
  没有任何 CAN/UART 业务派发。不允许通过强行置位能力绕过此边界。
- 实验 boot_id 只保证本次上电期间重新初始化不重复，不具备跨上电唯一性。
  必须实现可靠的启动 nonce/计数方案与本地保护后，才能进入可 Arm 阶段。
- 配置提交后不能热改；回落 PREOP、AL 错误确认均不清除业务保护。
  当前需显式硬件/ESC 复位、重新初始化并重新发送全部配置。自动重连恢复尚未实现。
- SPI IMU/加热仍禁用。板上 UART5 只有 RX，因此逻辑通道 5 的 `tx_bytes` 必须为 0；
  `onboard_imu` 必须 false。接口掩码表示板上可布线的能力，不表示后端已完成。

## 同源 SII / ESI 与固定 RAM 分区

`apps/ethercat_slave/gateway.json` 是设备身份和物理资源约束来源，
`tools/generate_gateway_esi.py` 同时生成 SSC 宏、2048 B RAM SII 镜像与 ESI。
主机 YAML 仍决定启用哪些 CAN/UART、槽数和有效数据长度，修改容量不需要重编从站。
设备 JSON 是板级资源上限，不能通过主机 YAML 扩大硬件容量。

| SM | 起始地址 | 单缓冲容量 | 模式 |
|---|---|---|---|
| 0 | 0x1000 | 128 B | 主机到从站邮箱 |
| 1 | 0x1080 | 128 B | 从站到主机邮箱 |
| 2 | 0x1100 | 至多 1024 B | Rx，三缓冲、看门狗 |
| 3 | 0x1d00 | 至多 1536 B | Tx，三缓冲 |

Rx 三缓冲到 0x1d00 之前，Tx 三缓冲到 0x2f00 之前；SII 声明的固定地址不能随 YAML 移动。
启动读取 ESC `0x0006` 的 RAM KiB 数，至少需要 8 KiB DPRAM，否则拒绝启动网关。
此运行时检查不是已经实测 RAM 寄存器的声明。SM 地址、长度、模式与启用位在进入 SAFEOP 前复核。
过程数据从空默认 PDO 起步，IgH 根据 YAML 下载真实映射，不保留计数器/旧 MPC 映射。

实验身份 VID=0、PID=0x44544331、Revision=0x00010000，与计数器 PID=0x57425231 区分。
VID=0 不可用于生产，主机正常控制运行时会拒绝它；保留该限制。
刷入新固件后须让主站重新读取 SII，不能继续使用旧身份、旧 SM 地址或缓存。
SII 写入仍只修改 RAM，不写 Flash/OTP。

生成文件位于构建目录 `ssc_gateway/`，ESI 归档副本在
`apps/ethercat_slave/esi/DUST_HPM5E31_Gateway.xml`。XML 已做内容契约检查，
尚未做 ETG XSD/一致性认证，也未实测 IgH 对新 SII 的枚举。

## 构建与离线验证

```sh
west build -b dust-hpm5e31 -p always
DUST_HOST_ROOT=/path/to/dust-control ./scripts/test_protocol.sh
./scripts/test_ethercat_ssc.sh
```

目标编译：ROM 121716 B，主 RAM 171580 B（256 KiB 的 65.45%），另有 AHB SRAM 10 KiB。
共享映射描述、按实际业务条目分配元数据，避免每个对象各占 255 条描述。
`Core`、映射、描述池和过程影像均静态持有，不放在 4 KiB 主线程栈。

原生 SSC 测试直接编译真实 `coeappl.c` / `objdef.c`，仅替换 ESC 寄存器与时钟边界，
并把整数宽度固定为目标机的 32 位 ABI。ASan/UBSan 覆盖：
配置/映射乱序、多个 PDO、真实 SDO 状态/位宽/Complete Access 校验、CRC 不一致、
UART5 非法 TX、SM 地址/看门狗、业务描述位宽、SAFEOP Ready、OP Discover、
Arm 拒绝、故障锁存与显式重新初始化。另检查生成 ESI/SII/SSC 的身份和 SM 地址一致。
这些测试不模拟 ESC 三缓冲电路、实际网络时序、IRQ 延迟或驱动输出。

下一步接 CAN/UART 后端与本地保护、可靠 boot_id、状态分页，再做新协议硬件枚举和周期验证。


## MCAN 后端前置条件（2026-10-27）

原 USB 使用 CASTCAN 的硬件批量发送接口；5E31 的 MCAN 旧驱动只处理 buffer 0，
不能直接承接同样的批次与保护语义。锁定的 hpm_support 已增加
`drivers/can/hpmicro_mcan_batch.h`：整批验证/装载、单次 TXBAR 提交、整批完成、
有界撤销、停止后令牌回收，以及旧撤销快照与新批次的 generation 隔离。
不将应用降级为循环调用单帧 `can_send()`。

接口与验证边界见工作区 `hpm_support/docs/MCAN_BATCH.md`，原生合约已纳入
`test_protocol.sh`。此步骤只补齐支持驱动，**EtherCAT 应用还未调用新 MCAN 接口，
CAN/UART 后端、持久 boot_id 和能力 bit31 尚未接入**。未刷板或使能电机。
下一步将先接来源绝对截止时间、MCU 白名单与保护帧队列，再处理 UART 确认和持久启动标识。
