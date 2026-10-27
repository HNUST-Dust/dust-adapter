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
- 已接入 MCAN 硬件批次、UART 原生 idle、MCU YAML 白名单和本地保护帧。
  初始化全部成功、启动计数持久化并读回后才声明 capability bit31，允许 Arm；
  初始化失败不创建可控制的网关核心。实验 VID=0 的主机运行限制仍保留。
- boot_id 来自芯片 UID 派生值与预留 Flash 中的递增计数，每次初始化先提交再暴露。
  计数损坏、读写失败或耗尽均拒绝启动；清空/回滚计数区会破坏跨启动去重保证。
- 配置提交后不能热改；回落 PREOP、AL 错误确认均不清除业务保护。
  当前需显式硬件/ESC 复位、重新初始化并重新发送全部配置。自动重连恢复尚未实现。
- SPI IMU/加热仍禁用。板上 UART5 只有 RX，因此逻辑通道 5 的 `tx_bytes` 必须为 0；
  `onboard_imu` 必须 false。接口掩码由实际初始化成功的端口生成。

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

目标编译：ROM 167188 B，主 RAM 201452 B（256 KiB 的 76.85%），另有 AHB SRAM 10 KiB。
共享映射描述、按实际业务条目分配元数据，避免每个对象各占 255 条描述。
`Core`、映射、描述池和过程影像均静态持有，不放在 4 KiB 主线程栈。

原生 SSC 测试直接编译真实 `coeappl.c` / `objdef.c`，仅替换 ESC 寄存器与时钟边界，
并把整数宽度固定为目标机的 32 位 ABI。ASan/UBSan 覆盖：
配置/映射乱序、多个 PDO、真实 SDO 状态/位宽/Complete Access 校验、CRC 不一致、
UART5 非法 TX、SM 地址/看门狗、业务描述位宽、SAFEOP Ready、OP Discover、
Arm 拒绝、故障锁存与显式重新初始化。另检查生成 ESI/SII/SSC 的身份和 SM 地址一致。
SSC 后端桩分别验证缺少保护拒绝 Arm、保护就绪允许 Arm/派发，以及 OP 空转不重复发布。
另有真实 IO 后端测试，见下节。
离线检查不模拟 ESC 三缓冲电路、实际网络时序、IRQ 延迟或物理输出。

## CAN/UART 后端与配置

`src/io_backend.cpp` 在 FreeRun 线程中串行处理协议动作和队列；ISR 只投递接收块、
完成状态及故障标记。OP 每个新 Rx 影像只发布一次 Tx，避免主循环空转导致 UART 上行窗口轮转。

- DTS aliases `dust_can_0..3` 和 `dust_uart_1..6` 绑定物理接口。
  主机 YAML 决定 PDO 的启用端口和容量，仍经 0x2100/动态映射下载。
- MCU `config/can_devices.yaml` 决定波特率、收发白名单、保护帧及更严格的命令期限，
  构建参数 `-DDUST_ADAPTER_CAN_CONFIG=/absolute/profile.yaml` 可选择其他配置。
  主机不能通过 PDO 放宽白名单或覆盖本地保护；更改 MCU 配置需要重新编译固件。
- MCAN 每路发送队列 32 帧，每批最多 16 帧；整批装载后一次 TXBAR 提交。
  使用最早绝对截止时间，驱动在 TXBAR 前再次检查，过期整批不提交。
  完成表示整批终结，不表示电机执行成功；接收不重传，仅保留当前 PDO 容量允许的最新帧。
- 本地停止先丢弃业务队列，有界撤销在途批次，再提交不可覆盖的 YAML 保护帧。
  撤销失败停止该控制器并记录错误，不再提交业务；其他总线继续尝试保护。
  已经上总线的帧无法撤回，保护帧能否到达仍取决于接线、总线状态和接收设备。
- UART 每路 2×128 B DMA RX 缓冲、4 个待收块；128 B idle 块按 YAML PDO 容量拆分。
  TX 为 4 个完整块的队列，每块最多 512 B，完整复制后才 Commit/ACK；队列满时不 ACK，等待重传。
  TX DMA 完成后还检查 FIFO 排空，避免下一次 DMA 重置 FIFO 丢失尾部字节。
  未交付 RX/TX 超过 20 ms 进入保护。停止丢弃软件队列并撤销 TX DMA，已上串行线的字节不能撤回。
- UART5 只接收 DBUS，沿用板级 100000 baud 并配置 8E2；其余 UART 沿用 DTS 波特率。
  所有端口启动原生 1000 us RX idle。115200 baud 对应 116 bit；高于硬件 255 bit 范围时
  初始化拒绝，不会静默钳位。SPI IMU/加热保持关闭。

诊断请求 target 低 2 位为类别，高 6 位为页：0=网关（channel=0），1=CAN（0..3），
2=UART（1..6）。每页返回 3 个 u32：

| 类别/页 | 数据 |
|---|---|
| 网关/0 | 业务状态、保护签名、MCU 最大命令期限 us |
| 网关/1 | 保护错误计数、IO ready、已初始化 |
| CAN/0 | 接收帧数、提交帧数、成功完成批数 |
| CAN/1 | 丢弃帧数、错误计数、待发帧数 |
| CAN/2 | 控制器状态、TX error counter、RX error counter |
| UART/0 | 接收块数、丢弃块数、成功完成 TX 块数 |
| UART/1 | 错误计数、TX busy、待发块数 |

无效页返回 -ENOTSUP，无效类别/通道返回 -EINVAL。

## 启动计数存储

`boot_identity.cpp` 只读取 OTP UID，不烧写 OTP。NVS 使用 `ethercat_eeprom_partition`
（0xd0000、64 KiB）前 8 KiB，即两个 4 KiB 扇区；不与 RAM SII 镜像共享数据。
链接区通过 ROM_END_OFFSET=0x30000 止于 0xd0000，避免代码增长覆盖存储区。
记录包含版本标记、UID 标签、64 位计数，启用 NVS 数据 CRC。首次运行会初始化这两个扇区；
随后每次网关初始化写入递增计数并读回，成功后才对外提供非零 boot_id。
这一步发生在 CAN 启动保护已交给硬件之后，但不是整个上电路径的延迟保证。

正常重启与显式 ESC 重新初始化都会取得新标识。擦除、恢复旧 Flash 备份或更换存储内容
可能复用计数；它不是不可回滚的 OTP 单调计数器。不要把启动计数区作为普通清理区擦除。

## 离线测试与下一步

`./scripts/test_ethercat_io.sh` 直接编译真实 IO 后端、UART 通用入口和启动计数实现，
仅替换设备/队列/时钟/NVS 边界。ASan/UBSan 覆盖：启动保护先于计数提交、完整 UART
接纳与 ACK、队列满重传、重复报文、上行分片、CAN 最新窗口、租约过期撤销、最终提交超时、
状态分页以及 NVS 读写/读回失败、损坏、无效 UID 和计数耗尽。

hpm_support 原生测试另直接提取 MCAN 提交函数体，验证装载过程耗尽租约后不会写 TXBAR，
且释放提交令牌、不伪造完成回调；UART 测试覆盖原生 idle ISR、DMA 部分发布及回调重启。
驱动细节见工作区 `hpm_support/docs/MCAN_BATCH.md` 和 `UART_NATIVE_IDLE.md`。

这些均为离线证据，尚未刷入本次固件。下一步在台架依次验证新 SII 枚举、YAML 动态映射、
Discover/Arm、CAN 空载收发、UART 短帧与连续流、断线/超时保护，再验证 1 kHz 多端口压力。
实验 VID=0 仍需专门的台架主站入口；正常控制运行时的身份检查未放宽。
