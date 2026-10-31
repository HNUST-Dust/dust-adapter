# 板载 IMU HXY 数据边界

参考 `/Users/panpoming/Documents/zephyr_projects/wbr_control_ws/dust-adapter/drivers/imu/onboard_imu.cpp`；所提供的 `hpm_support` 板级 DTS 只记录接线，不包含协议实现。参考驱动使用身份寄存器 0x01、期望 0x6a；从 0x0c 连续读取 24 字节，传输缓冲区含首个命令字节共 25 字节。温度寄存器为 0x22/0x23，大端有符号 16 位，摄氏度为 raw/512 + 23。

Adapter 负责芯片初始化、DMA 采样、时间戳和用于本地温控的温度换算；其他原始字节完整转发，交给 dust-control 解析。现有 `io_backend.cpp` 和 EtherCAT PDO 已保留 25 字节原始 SPI 传输及温度字段，SPI3 使用 XDMA 6/7，真实芯片身份和双端实机兼容性尚未验证。不要启用加热或把原始数据当已验证温度。

独立只读 DMA 诊断同时读取 HXY 0x01 与标准 ICM-42688-P 0x75 身份寄存器，以及 HXY 温度原始寄存器；未初始化传感器，温度结果仅用于检查传输。网关与独立诊断均使用 XDMA 6/7；ESC DMA 与 SPI DMA 不可同时启用，Kconfig 已限制。实机身份确认前不得使用身份不匹配的初始化寄存器序列。
