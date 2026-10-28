# 多线程阶段分析与事件唤醒实验

保留多线程、CPU word，DMA关闭。第一项实验只改变CAN接收线程的空闲等待方式，
不同时改变线程优先级、CAN协议、YAML/PDO、ESC搬运宽度或控制逻辑。

## 已有分段基线（修复后、64条、带v2和MCAN观测）

| 阶段 | 平均 us | P95 us | 解释 |
|---|---:|---:|---|
| ESC读96 B | 16.334 | 46.333 | 含可能的中断/调度干扰 |
| Core.Process | 26.121 | 58.583 | 校验、摘要、租约与动作解析，不等于纯算法成本 |
| CAN提交→反馈回调 | 300.981 | 396.417 | 已包含发送、总线/设备应答、RX处理，不能再叠加TX完成耗时 |
| CAN回调→成功入队 | 8.764 | 11.917 | IRQ内队列路径及观测 |
| 入队→出队 | 27.986 | 57.542 | 工作线程调度等待与get开销 |
| 出队→工作快照 | 4.027 | 6.583 | 有界排空/快照处理 |
| 工作快照→发布 | 59.974 | 99.667 | 缺少主线程取得快照边界，尚不能拆分等待/复制/Core.Publish |
| 发布→输入映射 | 5.751 | 8.208 | SSC路径 |
| ESC写400 B | 54.468 | 84.625 | PDI访问本身较长，可能被线程/IRQ抢占 |

数据来自dust-control的mcanfixed-timing.csv。不能把嵌套阶段和不同发布路径直接相加，
也不能用这64条带观测数据与后续无观测2000条RTT直接归因比较。
优先检查等待和无效调度；CRC查表等协议内部优化可在独立剖析后再做，避免同时改动。

## 代码改动

- `DUST_ECAT_CAN_RX_POLL_US`：100为控制，0表示K_FOREVER仅事件唤醒；默认暂保留100。
  IRQ完成入队和时间戳后立即给信号量，不人为增加收包等待。
- SSC owner 在ServiceCanReceive/CaptureCanFeedback的短锁内按最老capture年龄检查过期，
  无需高优先级接收线程定时醒来。仅在达到20 ms边界时扫描清理并递增快照generation。
  不修改Core持有的已复制数组，不轮转UART窗口；Stop/Arm同步清理age缓存和旧队列。
- 可选 `DUST_ECAT_SCHED_STATS`，默认n。`dust_ecat_sched_stats` 为28 B、7个u32：
  version、poll_us、active_wakes、active_timeouts、active_empty_wakes、active_changes、
  ssc_expirations。计数只覆盖Active采集阶段，包含Arm初始唤醒，不是CPU占用率。
- DTR1升级v3：40 B头、128条192 B记录，在原18点后附加CAN_SNAPSHOT（SSC复制完成）。
  旧点编号不变，可拆分capture→can_snapshot和can_snapshot→publish。
  主机解析同时兼容v1/v2/v3，已有v2真实数据重解码逐字节一致。

原生回归含纯事件线程、SSC无工作线程唤醒时的过期、32-bit捕获时间回绕、保护、UART窗口和
并发快照。压力用例明确断言Active，并放在粘滞故障注入前，避免只在Protected状态通过。

## 待执行实验

同源码、同优先级（worker0/main1），分别构建100 us轮询与0纯事件，两份实际.config仅差
DUST_ECAT_CAN_RX_POLL_US。先各64条带v3定位，再按poll/event/event/poll各1000条无DTR1
测整体RTT；所有镜像均保留相同的轻量调度计数，DMA/MCAN诊断关闭，CAN3 ID3失能白名单。
125 us PDO、10 ms命令间隔、FIFO40 CPU7、mlock及临时ASPM恢复一致。

每轮主站退出后导出计数，带trace轮另外导出DTR1，再切换固件；连续三次唯一LAB从站PREOP
后放行，NUC脚本还会复核枚举状态。实验尚未完成，不预判纯事件一定更快。
若有效，再单独确认默认关闭计数时的结果，并决定是否调整默认值。
