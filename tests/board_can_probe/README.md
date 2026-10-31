# CAN 被动监听工具

独立固件，运行后替换 EtherCAT 网关。仅设置 1 Mbps 被动监听、每秒输出接收帧计数及控制器状态，不发送帧也不 ACK；因此总线上必须有其他节点维持通信。仅统计标准帧，帧计数为 0 不能单独判定硬件故障。输出到调试器 SEGGER RTT，不使用 UART2/5。

在 workspace 根目录运行（替换 CAN_CHANNEL 为 0、1、2 或 3）：

```sh
export PATH=/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5
west build -p always -s dust-adapter/tests/board_can_probe -b dust-hpm5e31 -d dust-adapter/build/board_can_probe -- -DCAN_CHANNEL=3
west flash --skip-rebuild -d dust-adapter/build/board_can_probe
```

烧录前先确认 EtherCAT 主站不活动；测试结束须恢复网关固件。本工具不向电机发送控制命令，也不验证发送功能。
