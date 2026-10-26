# 源码组织与收发方向

统一以 MCU 为参照：Rx 为进入 MCU，Tx 为离开 MCU。USB 描述符和 wire 类型
保留 IN/OUT：IN 为 MCU 发往主机，OUT 为主机发往 MCU。

| 文件 | 职责 |
|---|---|
| `gateway_runtime.*` | 初始化协调、USB 请求分发、会话许可与停止保护 |
| `usb_protocol.hpp` | USB 报文字节布局、字段访问、校验和会话策略 |
| `usb_reports.*` | 回复及 CAN/UART/IMU 上报的汇合、排队和 USB 发送 |
| `imu_reports.*` | IMU 采集循环、温度提交和 USB 上报包构造 |
| `drivers/usb_transport.*` | USB 原始收发、端点、描述符及连接生命周期 |
| `drivers/can_port.*` | CAN 总线接收、发送 FIFO、批次及完成回调 |
| `drivers/uart_port.*` | UART 接收、发送队列及字节流上报 |
| `drivers/uart_receive.*` | 完整 UART 接收启动过程与平台扩展适配 |
| `drivers/imu_sensor.*` | HXY IMU 寄存器、SPI 与异步采集 |
| `drivers/imu_heater.*` | 温控任务、加热 PWM 和保护 |
| `drivers/board_indicators.*` | LED 与蜂鸣器状态指示 |

协议唯一定义入口为 `usb_protocol.hpp`，不存在旧文件名转接层。

主机命令：USB transport Rx → gateway runtime → CAN/UART port Tx。
外设反馈：CAN/UART port Rx 或 IMU reports → USB reports → USB transport Tx。

同一模块的收发留在同一 port/transport 中，不强行拆成成对文件。
RxLoop/TxLoop 用于线程循环，Callback 用于驱动回调，queue 用于消息队列。
