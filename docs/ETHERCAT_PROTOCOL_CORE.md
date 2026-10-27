# EtherCAT 协议核心

2026-10-27。代码位于 `src/ethercat/`，与 Zephyr、SSC、IgH、CAN/UART 驱动分离，
C++17、无异常、无 RTTI、运行时不分配堆内存。交叉测试对齐主机提交 `c2ad8cb`，
并确认 NUC 上布局/线格式/会话代码 SHA256 与本次测试版本一致。遵循主机 dust-control 当前的
`docs/ETHERCAT_RUNTIME.md` 与 `src/hardware/ethercat/pdo_config.cpp`。

SSC 应用现已通过自有对象字典和周期回调调用本核心；构建默认生成 DTC1 实验网关，
详情见 [SSC 接入与边界](ETHERCAT_SSC_GATEWAY.md)，CAN/UART 后端和本地保护现已接入，初始化成功后允许 Arm；尚未实板验收。
此前 1 kHz 实板计数器测试不能作为新协议核心的硬件验收。

## 配置与布局

`ConfigurationTransaction::Write()` 接收 0x2100:1～22 的 u32 值，顺序任意，
各字段先检查宽度/范围，再写 staging。`Commit()` 必须收齐所有字段，同时验证：

- 周期 1000 us、控制上限 20000 us；来源各 0～16 CAN 槽，反馈总线各 0～32 槽，
  每路 UART 的两方向各 0～512 B，IMU 为 0/1。
- 映射必须覆盖实际支持的 CAN/UART/IMU 能力；SPI IMU 关闭的板应提供 `imu=false`
  的 Limits，主机 YAML 同时设置 `onboard_imu: false`。
- 两方向影像尺寸和三缓冲预算满足由板级集成提供的 Limits。预算必须预先扣除邮箱与保留区，
  本核心不猜测 ESC RAM、SM 起始地址，也不代替下一阶段的地址重叠检查。
- 与实际对象映射的 PDO 索引/边界、条目索引、subindex、位长和偏移逐项一致。
  实际映射必须来自 SSC/对象字典，不允许用候选布局冒充实际配置。
- CRC32 与主机下发签名一致。签名包含与主机相同的字段名、顺序、分块及 PDO 分组规则。

`BuildLayout()` 生成用于比对的候选布局；不执行硬件配置。单方向 UART 仍保留反向 ACK 头。
字节数组按最多 30 B 分条目，255 条目后创建下一 PDO。空 IMU/CAN/UART 区不占影像。
主机默认布局为 Rx 976 B / Tx 1536 B；文档中的最小底盘示例为 208 B / 656 B。

配置提交后核心进入 Ready，不能在 Ready/Active/Protected 上再次覆盖配置；
重新配置必须由集成层先完成停机、清队列和重新初始化，不能热更改活动影像。

## 状态机、授权和期限

- 构造时要求板级提供非零且不重复的 boot_id；包括故障恢复重建核心，也必须更新标识、
  清理旧队列和过程影像，不能复用旧 boot_id。核心本身不伪造随机启动标识。
- 未配置时不接受业务；Discover 可以使用 expected_boot_id=0，Arm 必须回显当前 boot_id。
- 默认 `Protection::installed=false`。只有集成层明确安装并检查本地停机回调和 CAN
  白名单验证后，才能在构造参数中声明 `installed=true`。没有这些条件，不声明 capability bit31，也拒绝 Arm。
- `Process()` 先验证完整影像，再提交会话、来源序号和 ACK 更新。返回失败时 Actions
  全部无效；同周期后面的坏区不能使前面的 CAN 批次泄漏到后端。
- Active 锁定非零 session_id 与 active_source_mask；Arm 幂等，但不解除保护。
- exchange_sequence 按 u32 半空间规则比较；完全相同的重复影像不派发、不续任何期限。
  同 exchange_sequence 改写内容会进入保护。
- 每个注册 CAN 来源首次等待不超过 20 ms；新 source_sequence 才生成一次 CanBatch。
  remaining_valid_us 建立绝对 deadline_us；相同来源序号不续期，内容变化/乱序被拒绝。
  空批次可维持该来源活性，UART、另一来源、影像交换均不能代替它。
- Stop、来源/通信期限耗尽、传输失效、旧会话/boot、非法影像或后端错误均锁存 Protected，
  同步调用本地 stop 回调一次。新 Arm/Discover 不清故障；本阶段恢复需要集成层明确停机后
  重新初始化核心，不提供普通 PDO 清保护功能。恢复策略需在集成时明确落地。

核心不可复制/移动；Active 实例销毁时也触发本地停机，保护回调上下文必须比核心存活更久。
集成层须以单个执行上下文拥有核心。每周期和无新 RxPDO 时都调用 `Tick(now_us, transport_valid)`，
transport_valid 应结合 AL 状态、有效过程数据和 SM 看门狗，而非只检查网线已插入。
`Publish()`/`QueueUart()` 也检查时间期限，但不能代替 Tick 对运输失效的检测。

CanBatch 表示协议接纳，不表示 CAN 发送完成。后端实际出队时仍须检查 deadline_us、
执行本地白名单与保护约束；任何失败调用 `Protect()`，并清理待发队列。
应用层错误不能只丢日志后继续工作。

## UART、反馈和诊断

每路独立四块发送窗口：序号从 1 开始，累计 ACK，只确认已发送的窗口成员，
重复 ACK 不前移窗口，未确认块按顺序循环发布。队列满返回 false；调用方累计丢弃，
有真实流缺口时设置 discontinuity，不能默默拼接半帧。

下行只交付期望的下一块。重复和缺口不重复交付、不推进 ACK。必须先把完整块复制到
UART 后端并预留完整空间，再调用 `CommitUart()`；不能先确认再尝试入队。
Actions 中的 UART 字节视图仅在输入影像保持不变时有效，同周期完成接纳/复制/Commit。
上行队列仅在 Active 接受新块；20 ms 未能完成块交付/确认会进入保护。

`Publish()` 编码公共头、每总线有界 CAN 原始事件窗口、UART、可选 IMU 和 224 B 诊断，
publish_sequence 每次成功发布推进，所有线格式均显式小端，不复制宿主结构体 ABI。
CAN 上行不做可靠重传；调用方提供 batch_sequence、总线事件序号、捕获时间和累计丢弃。
IMU 保留 25 字节原始数据和 binary32 温度；能力存在不代表当前板已启用 SPI IMU。
时间使用从站单调时钟，线上的低 32 位支持回绕；不把主从时钟视为已经同步。

状态请求通过 Actions 交给后端，`CompleteStatus()` 只接纳当前请求的结果，
回显序号和结果落在主机要求的位置。CAN/UART 详细统计的获取和状态分页提供器在后端集成时完成。

## 验证

```sh
./scripts/test_ethercat_protocol.sh
# 与真实主机生成器交叉检查；仅需本机编译器、yaml-cpp，不启动主站。
DUST_HOST_ROOT=/path/to/dust-control ./scripts/test_ethercat_protocol.sh
# 含原 USB/CAN 保护/温控合约的完整回归：
DUST_HOST_ROOT=/path/to/dust-control ./scripts/test_protocol.sh
west build -b dust-hpm5e31 -p always
```

已通过 ASan/UBSan 原生合约测试，以及 504 组主从布局比对：414 组有效布局逐条目/
PDO 分组/CRC 相同，90 组超预算配置双方均拒绝。覆盖重复/乱序/回绕、旧 boot/session、
坏映射和不完整配置、来源独立到期、首次等待、缺失本地保护、Stop、完整影像截断、
部分坏包不派发、UART 重传/缺口/未发送 ACK/单方向容量/通道隔离及时间回绕。

本机 x86_64 `sizeof(Core)=25880 B`，为固定上限实例开销，接入时应静态/持久存储，
不能作为 4 KiB 线程栈上的临时对象。当前 RISC-V -Os 静态栈报告中 Process 自身
240 B、Publish 自身 128 B、布局 Image 自身 224 B；不含下层 printf/libc 和保护回调栈，
也不是实机栈水位或周期耗时验收。

SSC 对象字典/动态映射、CAN/UART 后端、本地保护、持久 boot_id 和诊断分页已接入。
下一步做新协议主从硬件交换测试；故障恢复仍需显式重新初始化。


## 动态 PDO 写入与过程 RAM 分配

`pdo_dictionary.hpp` 提供独立于 SSC 的 CoE 映射暂存器，下一阶段的 SSC 回调复用它，
而不是另写一套长度计算。支持 Rx `0x1600..0x1604`、Tx `0x1a00..0x1a04`，
以及 `0x1c12/0x1c13` 分配对象。SDO 下载需遵循：subindex 0 写 0 禁用，
写入全部条目，再以 subindex 0 发布条目数。映射条目为 32 位，分配条目为 16 位，
subindex 0 为 8 位；不支持 Complete Access。禁用会清除上一轮条目，禁止沿用遗漏字段。

`Snapshot()` 重建实际条目、逐字节偏移和 PDO 分组，两个输出只能在返回 None 后使用。
把它交给 `ConfigurationTransaction::Commit()`，同时检查全部 22 项配置、CRC 和真实映射；
通过后才 `Freeze()`，冻结期间所有写操作均拒绝。仅返回 PREOP、停止周期访问后才能 Clear。
所有对象固定分配、无堆分配；暂存器需静态存放，不放在线程栈。

`AllocateProcessMemory()` 按 Rx/Tx 各三个缓冲区计算地址，区域起始按 4 字节对齐，
防止邮箱重叠、算术回绕和超出调用方已验证的 ESC RAM 上限。输入 begin 已扣除邮箱和保留区。
例如 begin=0x1100、end=0x3000、Rx=976 B、Tx=1536 B，得到 Rx=0x1100、
Tx=0x1c70，最后使用地址之后为 0x2e70。该示例是给定预算的计算，**不代表已确认芯片 RAM 容量**。
后续 SSC/SII/主站必须共同采用同一 SM 物理地址方案，不能只在从站悄悄改地址。

原生测试覆盖未禁用即写、部分写入、重配遗漏、跨方向映射、位宽错误、冻结、地址对齐、
三缓冲边界和超预算。504 组真实主机布局交叉测试同时重放主机映射 SDO，
并验证配置提交及错误映射拒绝。此模块现已由 SSC CoE 回调调用；应用启动时读取 ESC
RAM 容量并校验固定分区，实验 SII/ESI 已同源生成。新固件尚未刷入硬件，
实际后端和本地保护的验证边界见 SSC 接入文档。
