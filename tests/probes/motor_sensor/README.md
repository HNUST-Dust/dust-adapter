# DTG1 到达间隔验证

本目录的 `contracts.cpp` 检查 60 B 六命令解码、静态路由及失败时不部分写入输出：

```sh
c++ -std=c++17 -Isrc tests/probes/motor_sensor/contracts.cpp -o /tmp/dtg1-contracts
/tmp/dtg1-contracts
```

运行期测量工具在 `../device_pdo/host_bench.cpp`，已移除旧时序跟踪与控制器影子执行分支。它单独在 Linux 上位机构建，链接 dust-control 的 dust_ethercat、dust_ethercat_pdo、dust_imu、dust_remote，使用 C++20，包含本仓库 src 和 dust-control/src。正常固件不编译此工具。

运行参数（末尾必须显式指定 safe-can，否则立即退出）：

```sh
arrival-only 60 sensor60 shadow1k motors-imu-event live-full combined hi91-compact dm-disable 250 safe-can
```

历史位置参数为兼容旧脚本保留；实际是 125 µs EtherCAT 周期、1 kHz 六条真实安全 CAN 命令。不会执行平衡算法，也不再由从站生成独立轮询。四关节只发失能帧，两轮只发零电流帧。测试结束发送 Stop。需要 CPU 3 与 FIFO 80 实时调度权限。

正式固件已删除测试计数器，因此无 can_passthrough 完成报告；JSON 中仍有主站命令批次数、各反馈到达间隔最小/p50/p99/最大值和交换错误计数。

正式版记录与一分钟原始数据见 `docs/gateway-dtg1-release-2026-11-07.md`。

## 可选源到达间隔测量

默认 CONFIG_ECAT_SOURCE_ARRIVAL_STATS=n，代码不进入正常固件热路径。手动使用 config/labs/source-arrival.conf 构建测量镜像时，才记录 CAN 帧起始硬件时间戳/MCU 回调间隔和 IMU DRDY/DMA 完成间隔，收到 Stop 后冻结并报告。正常配置须清空 EXTRA_CONF_FILE 并重新构建、部署。

CAN 硬件时间戳分辨率为 16 个标称位时间；当前 1 Mbps 下为 16 µs，16 位计数器约 1.048576 s 回绕。统计以相邻回调间隔检查回绕歧义；超长停顿不能用该计数器归因。源 count 是相邻间隔个数。主站的 sof_to_driver_sample_max_us 包含 CAN 帧在总线上的传输时间，采样点在驱动读取接收 FIFO 后，不能当作纯 IRQ 等待时长。早期本轮原始 JSON 将这一字段命名为 sof_to_callback_max_us，含义相同，以本说明为准。

这些源统计用于诊断；正式性能以关闭测量的同配置长测为准。

## 四总线隔离（待实板验证）

主站以 DUST_CAN_ISOLATION=1 编译，使用 DTG2 签名。右轮 CAN1、左轮 CAN2；右关节 CAN0、左关节 CAN3。固件以 config/labs/can-isolation.conf 和同名 overlay 构建，自动选择对应白名单。不得在旧接线下当作正常版使用。当前仅本地构建与合同测试通过，部署状态见 docs/gateway-dtg1-optimization-2026-11-07.md。
