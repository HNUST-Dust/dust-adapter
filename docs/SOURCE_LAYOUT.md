# 源码组织与线程归属

## 构建与职责

根目录只有一个固件构建入口：`CMakeLists.txt` + `prj.conf`；板级覆盖位于 `boards/`。
所有自有固件源码都位于 `src/`。外部授权 SSC 源码仍在 `external/ssc`，构建时组合到生成目录，不改写第三方源文件。

| 路径 | 职责 |
|---|---|
| `src/main.c` | HPM/PHY/SSC 初始化及 FreeRun 主循环 |
| `src/ethercat/slave/gateway.cpp` | SSC 与协议核心连接、PDO 发布、会话及看门狗 |
| `src/ethercat/slave/io_backend.cpp` | CAN/UART 中断接收、队列、发送批次、保护 |
| `src/ethercat/slave/boot_identity.cpp` | NVS 启动身份 |
| `src/ethercat/slave/hpm5e_esc_hw.*` | ESC 硬件适配 |
| `src/ethercat/slave/esc_dma.*` | 可选 DMA 实验，默认关闭 |
| `src/ethercat/slave/rtt_trace.*` | 有界 RAM 时间戳，默认关闭 |
| `src/ethercat/{core,layout,pdo_dictionary}.*` | 可原生测试的协议核心与动态 PDO 布局 |
| `src/drivers/uart_receive.*` | 当前共用的原生 UART idle 接收适配 |
| `config/ethercat_gateway.json`、`config/esi/` | 网关身份、生成配置及 ESI |

旧 `gateway_runtime`、`usb_reports`、`imu_reports`、USB port/transport 源码和
`src/legacy_usb/main.cpp` 留作迁移参考和原生协议测试，不在当前固件源文件清单中。
不再提供根目录 USB 构建模式；USB 固件使用原 USB 分支。

## 原 USB 多线程模式

Zephyr 的中断回调和工作线程分工值得复用：回调只记录数据、入队并唤醒消费者，
线程处理有界队列和硬件提交，不在中断中解析完整业务协议。

| 原模块 | 分工 | 当前直接复用的限制 |
|---|---|---|
| CAN port | 每总线发送线程，优先级 6、栈 1536 B，信号量/最长 1 ms 等待 | 当前主线程优先级 0，持续运行时低优先级线程不能及时执行；超时兜底也需重新设计 |
| UART port | 共享发送线程、栈 1536 B，队列与互斥锁 | 需保留 DMA 完成与实际串口空闲的区别，不能覆盖在途缓冲区 |
| USB reports | 独立发送线程、栈 2048 B，信号量唤醒 | EtherCAT 不能直接复用 USB 包发送模型，SSC 同步管理 PDO |
| USB transport | 独立接收线程、栈 1024 B，信号量唤醒 | EtherCAT 的会话和 PDO 配置有自己的生命周期 |
| IMU reports | 独立采集线程、栈 2048 B | 当前 5E31 暂停 SPI IMU，不创建此线程 |

这里的 1 ms 是无唤醒时的最大等待，不表示每次收发都固定等待 1 ms。
这些任务主要分离阻塞与生命周期，同一 CPU 上增加线程不会缩短 ESC 寄存器访问本身。

## 当前 EtherCAT 线程边界和优化判断

当前采用独立 CAN 接收线程 + SSC 主线程。`dust_can_rx` 优先级 0，栈 2048 B；
主线程优先级 1，继续唯一拥有 SSC、Core、软件发送队列、UART 窗口和 ESC 读写。
CAN/UART 中断异步入队，系统工作队列也是额外的内核线程。

最近 canfast 的 64 个样本中，CAN 回调到快照平均约 63.3 us，其中入队前约 9.6 us、
队列等待约 52.1 us、出队到快照约 1.5 us。ESC 输入写入约 50 us。
因此值得实验高优先级 CAN 接收工作线程，但这不能直接证明 RTT 会下降：
上一轮仅提前捕获快照把等待转移到了发布阶段，整体 RTT 没有稳定改善。

本次目录迁移保持调度和协议行为不变，便于用同一基线验证。
接收线程需要遵守以下边界并做 A/B 对照，不能让两个线程同时无锁排空同一反馈数组：

1. CAN IRQ 入队后以信号量唤醒高优先级接收线程，处理有界批次后阻塞；不能忙循环抢占 SSC。
2. 接收线程只拥有采集队列和私有快照。SSC/Core 和 ESC 读写仍归一个线程。
3. 用短临界区复制或带生命周期的双缓冲交接快照；当前 Feedback 中保存数组指针，
   不能让工作线程在 Core.Publish 读取期间覆写同一数组。
4. 配置提交、Arm、Protect/Stop 都必须同步采集 epoch 和旧队列清理；UART 的 ACK/窗口游标
   仍由 Core 唯一推进，异步完成回调不能重新发送已保护的命令。
5. 相对优先级必须显式设置：当前主线程 0，高优先级采集线程要能抢占，SSC 又不能饥饿。
   工作队列的合作式优先级同样需要考虑。
6. 比较 125 us 同条件 RTT、入队/出队、快照/发布、WKC、丢包及栈水位，确认端到端收益后才默认启用。

这保留了原 src 的模块和事件驱动思路，也避免未经验证地改变实时调度。

## 本次迁移验证

2026-10-28：完整 `scripts/test_protocol.sh` 通过，包括 414 个有效主从布局及 90 个拒绝布局、
实际 SSC、实际 IO 后端、UART 原生 idle、MCAN、ESC 字宽及 DMA 故障保护测试。
从根目录分别使用默认配置及 DM 白名单 + RTT trace 新目录构建 `dust-hpm5e31`，均通过。
DM trace 镜像 ROM 170860 B、RAM 229244 B，与迁移前相同；本次未刷写或执行实机 RTT。

## 多线程实验实现（2026-10-28）

沿用原 src 中队列 + 信号量 + 有界工作线程的模式。首轮独立 CAN 接收，
CAN/UART 发送继续使用当前异步硬件驱动，避免同时改变多个延迟环节。

- IRQ 完成入队及时间戳后唤醒 `dust_can_rx`；线程每次最多处理每总线 32 条。
  二值信号量合并唤醒，100 us 超时只用来淘汰过期快照，不是固定收包等待。
- 接收线程在短互斥区更新快照，SSC 复制至自己的独立数组后释放锁，再调用 Core.Publish。
  Core 持有的指针不会随接收线程更新而改变。锁不覆盖 ESC、CAN/UART 硬件提交。
- SSC 复制前的多次 IRQ 唤醒合并为最新容量范围内的记录，避免线程调度把一批反馈拆成
  多个互相覆盖的单帧快照。快照复制后，新一批覆盖此前的已消费快照。
- Stop/Protect 同步禁用采集、清空快照和旧队列；Arm 同步清空旧队列后重新开放。
  工作线程不调用 Core、SSC，也不自行发 CAN/UART 命令。
- 原生测试同时编译单线程与多线程后端；多线程边界用真实 std::thread、互斥锁、原子计数
  和条件变量，包括并发 IRQ 入队/快照发布、持有快照的稳定性、保护时仍有 IRQ 入队。
  原生测试不模拟 Zephyr 优先级与中断时延，性能仍需实板测量。

新目录实验构建：

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_can_thread -- \
  -DDUST_ADAPTER_CAN_CONFIG=$PWD/config/can_bench_dm_rtt.yaml \
  -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_DUST_ECAT_ESC_DMA=n
# 同源码单线程对照：独立目录构建，并恢复之前的主线程优先级。
west build -b dust-hpm5e31 -d build/dm_rtt_thread_control -- \
  -DDUST_ADAPTER_CAN_CONFIG=$PWD/config/can_bench_dm_rtt.yaml \
  -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_DUST_ECAT_ESC_DMA=n \
  -DCONFIG_DUST_ECAT_CAN_RX_THREAD=n -DCONFIG_MAIN_THREAD_PRIORITY=0
```

默认启用本次线程实验；不能在启用接收线程时把主线程优先级设为 0 或更高，编译会拒绝。
SPI IMU 仍关闭、DMA 仍关闭、PDO/YAML 布局不变。实机验证仍只允许 DM 失能帧。
