> 状态：living · 更新：2026-11-02 · 分支：ethercat

# 板载 IMU HXY 数据边界

参考实现来自主机侧的 HXY 板载 IMU 驱动；`hpm_support` 板级 DTS 只记录接线，
不包含协议实现。驱动使用身份寄存器 0x01、期望值 0x6a；从 0x0C 连续读取
24 字节，传输缓冲区含首个命令字节共 25 字节。温度寄存器为 0x22/0x23，
大端有符号 16 位，摄氏度为 `raw/512 + 23`。

Adapter 负责芯片初始化、DMA 采样、时间戳和用于本地温控的温度换算；
其他原始字节完整转发，交给 dust-control 解析。当前实现：

- 采样：`src/drivers/imu_sensor.cpp` 使用 SPI3 + XDMA 6/7，CS=PC21、DRDY=PC18，
  DRDY 中断记录采样时刻，异步 DMA 完成后再取最新样本。
- 转发：`src/ecat/io_backend.cpp` 每个发布周期取最新突发，
  EtherCAT PDO 保留 25 字节原始 SPI 传输和 binary32 温度字段，
  见 [PDO 协议](ethercat-pdo-protocol.md)。
- 温控：`src/drivers/imu_heater.cpp` 的系统工作队列按
  `CONFIG_DUST_IMU_HEATER_PERIOD_MS`（默认 10 ms）运行单向加热 PID，
  目标温度 `CONFIG_DUST_IMU_TARGET_C`（默认 45 ℃）。
  超过 200 ms 没有新温度样本即判故障并停止加热；
  `CONFIG_DUST_IMU_HEATER_ENABLE=n` 时只保留观测、占空比固定为 0。
- 编译开关：`CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW=y` 启用采样。
  原始数据与温度是否出现仍由主机 PDO 配置的 `imu` 字段决定。

## 诊断固件

`tests/` 下的只读 SPI DMA 探针同时读取 HXY 0x01、标准 ICM-42688-P 0x75
身份寄存器以及 HXY 温度原始寄存器；它不初始化传感器，温度结果仅用于检查传输，
不能用于温控。网关固件与探针均使用 XDMA 6/7；ESC DMA 与 SPI DMA 不可同时启用，
Kconfig 已限制该组合。

## 边界

- 真实芯片身份与 HXY 双端寄存器序列尚未在实板确认；身份不匹配前不得套用
  其他传感器的初始化序列。
- 25 字节原始数据是未验证量，不等于已标定姿态或温度；温控依赖的温度换算
  需要实板采样核对。
- 加热器的波形、升温效果与热模型尚未在实板验收，台架调参前不得把占空比
  当作已标定的控制量。
