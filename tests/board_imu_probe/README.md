# 板载 IMU SPI DMA 只读诊断

独立固件使用 SPI3 和 XDMA 6/7，循环读取 HXY 和标准 ICM 身份寄存器以及 HXY 温度原始寄存器，结果输出到 SEGGER RTT。程序不初始化传感器，温度原始值不能用于温控。测试固件不运行 EtherCAT 或加热控制。

在工作区根目录构建：

```sh
export PATH=/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5
west build -p always -s dust-adapter/tests/board_imu_probe -b dust-hpm5e31 -d dust-adapter/build/board_imu_probe
west flash --skip-rebuild -d dust-adapter/build/board_imu_probe
```

联机测试前确认 EtherCAT 主站不活动，结束后恢复网关固件。
