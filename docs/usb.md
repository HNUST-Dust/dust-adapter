# USB 网关

入口 `apps/usb_gateway`，只做硬件采集/转发与本地失联停止，算法在 NUC。

- HS Bulk，接口 0，OUT 0x01 / IN 0x81，HS MPS 512。
- 实际应用数据为 128B OUT 与 64B IN；Hello/Ready/Stop 为 64B。
- 协议版本、会话、递增序号、长度与电机地址均验证；无周期 ACK 或诊断页。
- CAN3 左 W/B/D，CAN2 右 W/B/D；配置在 `config/motors_params.yaml`。
- UART10 SBUS 100000 8E2，128B 非缓存异步双缓冲，独立 1000us RX-line idle
  部分发布。`CONFIG_WBR_UART10_LINE_IDLE` 保留旧名避免无意义行为变化，默认开启。
- UART2 HI91 921600 8N1，连续 DMA 环，保持 16B outstanding-write guard。
- 板载 SPI IMU 只发送原始 burst/温度，不运行姿态估计。
- 20ms 无有效新命令、USB deconfiguration 或 Stop：提交轮毂零电流与 DM Exit，
  清除会话，恢复必须重新握手。提交成功不等于实际电机停止时间已验证。
- 保留原 USB descriptor 及 namespace/参数名，保证现有 NUC 兼容；未迁入 PC/MPC
  业务路径。interfaces/ 中部分历史消息值类型是底层收发依赖，不启动业务模块。

协议权威头文件：`include/protocols/usb_bridge/bridge_protocol.hpp`。
主机镜像见 `wbr_control_companion/chassis_controller/include/wbr_bridge/bridge_protocol.hpp`；
变更 ABI 时须同步版本和两端测试，不能仅改一端。

当前固件属于固定六执行器配置。扩展成通用多设备适配器时，应另外设计地址/端口
配置与安全策略，不可删除白名单后继续沿用机器人停机语义而不说明。
