# 迁移记录（2026-10-22）

来源：NoneOfEver/wbr_control，清理后提交 afc2e2d；共享硬件文件使用迁移时
工作树内容，实际源文件 SHA256 见 migration_sources.json。没有把原仓库整体复制。

USB 从 src/usb_can_bridge 迁出，底层必要文件进入 drivers/ 和 include/。
旧的 PC/MPC 条件分支被移除，保留现有 raw Bulk 编译路径、调度优先级与 ABI。
没有迁入 chassis 控制算法。迁出前后清理版本 USB bin 曾匹配 SHA256：
0082e0e04bee8696384aeb37dfaf15c467f9191da56b5fbba5c5b8e2b92746c5。
上述一致性比较使用相同的本地 HPM 驱动基线。另用固定版本 HPM 加本仓库 UART 补丁
完成两个应用的独立 workspace 构建，未包含本地 CAN 实验追踪改动；USB ROM 因而减少
80 字节。固定依赖构建哈希见 build_verification.json，不与上述同基线哈希混用。

EtherCAT 按用户确认，仅迁出现有 HPM5E3Y FreeRun 计数器从站基础。现有
MPC slave core 与底盘 fallback 属于算法工程，不混作通用 EtherCAT CAN 网关。
SSC 实现与生成配置仅本地导入，没有重新许可或发布到本仓库。

原 Zephyr/HPM/CherryUSB/SDK 作为第三方依赖保留在 workspace，由 west.yml 固定版本。
HPM UART 功能补丁单独保存，未把用户工作树中的 CAN 追踪实验改动夹带进来。

迁移不自动刷写，原已运行固件不受影响。新仓库完成编译不表示已完成新硬件验证。

2026-10-22 分支拆分：本分支只保留 EtherCAT 从站基础；USB 位于 main/usb。
此前 USB 构建记录为拆分前的历史验证，本分支不需要 UART 功能补丁。

2026-10-22 后续：维护者确认厂商授权并明确批准公开 SSC，现将配置源码纳入
external/ssc，保留原许可；EtherCAT CI 升级为完整固件构建。此前外部导入策略为历史记录。
2026-10-22 后续：UART RX 修复已提交到 HNUST-Dust/hpm_support，
west.yml 固定到 a31c06940e8d11ee360b970c89523a7727b63fff；删除补丁与 CI/README 手工应用步骤。
旧版构建与补丁记录仅为历史记录。

2026-10-23：通用协议 v2 在 feature/generic-usb-adapter 独立开发，移除机器人消息、
参数生成、电机地址和周期槽位；UART 参数仍固定。v0.1/main 保留旧协议以供回退。
配套 dust-control 同名分支；详见 GENERIC_USB_MIGRATION.md。

2026-10-23 后续：按维护者要求引入 config/can_devices.yaml，构建时生成设备地址、
接收过滤器和默认 CAN 保护表；上位机不得清除或覆盖 CAN 基线。UART 参数仍固定，
电机协议解析和控制算法仍在 NUC。历史“完全原始全接收/主机上传保护”描述由此策略替代。

### UART 接收的平台边界

应用层 `peripherals.cpp` 通过 `platform::StartUartReceive()` 安装回调并启动
异步接收，不直接依赖芯片驱动扩展。统一入口位于
`src/drivers/uart_receive.cpp`：默认使用 Zephyr UART 异步 API；仅在
`CONFIG_UART_HPMICRO_RX_LINE_IDLE` 启用时，额外调用 HPM 空闲检测的准备与启动
接口。检测器硬件实现和引脚、定时器连接仍归 `hpm_support` 驱动与板级 DTS
所有。其他 board 不启用该配置即可使用通用路径，但仍须具备异步 UART 支持，
并单独配置端口映射及 DMA 缓冲区的内存属性。

### 单应用、多板卡配置约定

- 每块板通过应用 overlay 提供 `dust-uart-1..3`、`dust-can-0..3` 和
  `dust-usb` 别名。UART 别名可以缺省；CAN 配置引用的通道必须存在且启用。
  逻辑端口的 wire 数值保持不变，不再要求物理 UART 编号相同。
- HPM SPI、USB 和 PLIC 选项位于对应板的 `.conf`。
  CAN 调度直接使用 `hpm_support` 的 `hpm_can_send_batch()`，保留三帧硬件
  批次和整批完成回调，不提供单帧回退。新增平台必须明确提供等价能力，
  或单独设计、验证发送行为。
- DMA 缓冲区使用 Zephyr `__nocache`，板卡通过
  `CONFIG_DUST_DMA_BUFFER_ALIGNMENT` 指定对齐；须为二的幂，且满足具体 DMA
  与缓存要求。HPM6750 保持 64 字节。
- HXY 原始 IMU 由支持该器件的板卡启用；其他板不默认启用。换传感器不能
  直接复用寄存器表或原始突发协议，需要配套驱动和上位机解码。
- USB 仍使用 CherryUSB HS Bulk；其他平台需提供其 CherryUSB 端口及
  `usb_config.h`，不能仅靠新增别名完成移植。

### 扩展接口与 IMU 温控

逻辑 UART 4/5/6 分别连接 UART11/12/15（均为 115200 8N1），沿用 128
字节异步双缓冲。UART11 使用 XDMA 4/5，UART15 使用 XDMA 6/7，UART12
保留 XDMA 2/3。Ready 的能力位分别为 32/64/128，原有能力位不变。
上位机须同步协议头和客户端端口范围，才能发送新端口请求。

CAN0/1 初始化为 1 Mbps，设备表为空；不接纳发送，也不设置接收转发过滤器。
后续通过 YAML 明确添加 ID 和保护帧，不自动开放任意 ID。

RGB 指示：空闲蓝、已授权绿、传输或温控故障红；故障每秒短鸣 100 ms。
蜂鸣器使用 PWM0 通道 6、2 kHz；加热使用独立 PWM1 通道 0、20 kHz。

IMU 温度原本已在 IN 包偏移 48..51 中按 float（摄氏度）上报，序号位于
52..55；本轮不改变此布局。温控独立于 USB 会话，20 ms 更新一次，沿用旧
AHRS 的 45℃目标、kp=10 %/℃、ki=0.5 %/(℃·秒)、kd=0，最大占空比 100%。
内部使用 0..1 占空比，所以配置增益分别为 0.1 和 0.005。
板卡启用 `CONFIG_DUST_IMU_HEATER_ENABLE`，启动前先写零占空比；温度反馈超过
200 ms 未更新、启动后 1 秒仍无反馈、非有限温度、低于 -40℃、达到 80℃、
控制间隔异常或 PWM 写入失败均锁存关断，重启前不恢复。关断依赖任务调度及
PWM 驱动成功写入，不构成硬件独立温度保护。

当前仅经过离线测试与构建；新增接口、灯色、蜂鸣器、温控及关断须硬件验收。

### USB 设备身份

产品名称取 `CONFIG_BOARD`，厂商字符串为 `Dust`。HPM 平台只读 OTP shadow
中的完整 16 字节 UUID（不烧写 OTP），各 32 位字按小端顺序拼接。
序列号为 UID 的大写十六进制字符串；PID 使用这些字节的 CRC16-CCITT-FALSE
（多项式 0x1021、初值 0xffff、不反射、不做最终异或）。同一 UID 每次启动
生成相同 PID，但 16 位哈希可能碰撞，不能替代完整序列号。
UID 全零或全 0xff 时拒绝启动 USB，避免发布虚假的统一设备身份。
VID 暂保留依赖中的 `USBD_VID`（0x34B7），本轮没有分配新的 VID。
上位机不能继续固定匹配旧 PID 0xffff，须通过设备枚举和完整序列号选择设备；
udev 规则也应避免固定旧 PID。当前未同步上位机实现，未做硬件枚举验收。

### 旧兼容入口清理（2026-10-24）

删除 protocol.hpp 转接头、Packet/Request/Record 和物理 UART 旧别名；
删除 build.sh usb 参数、未使用的 USB polling 入口及 PC/MPC、AHRS 旧条件分支。
保护配置只允许选用 MCU 默认 CAN 表和补充 UART 停止动作，不再接受上传完整
CAN 默认表的旧确认方式。历史交接记录保留为追溯资料，不代表当前接口。

### EtherCAT 合并 generic USB 基线（2026-10-26）

将 feature/generic-usb-adapter 合入 ethercat。保留根目录 USB v4 应用的 CAN/UART、
保护配置、原始 IMU 和温控，以及 apps/ethercat_slave 与 external/ssc 的旧从站基础。
构建入口显式区分 ethercat/usb，默认选择 ethercat；双方都不自动刷写。
本次仅建立开发基线，计数器 PDO 尚未升级为 HPM5E31 网关协议。
