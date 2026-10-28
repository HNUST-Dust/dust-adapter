# 多线程 CAN 延迟定位

当前继续采用多线程方案，DMA 暂时关闭。先定位 CAN 提交→反馈回调新增约 6 ms，
消除该异常并复测后，再组合测试多线程 + DMA。

## 观测设计

原有 DTR1/v2/184 B RAM 时间戳保持不变，新增可选 MCT1 sidecar `dust_mcan_timing`。
每条记录按 boot/session/batch/CAN sequence 与原 RTT 数据关联，不修改 PDO/YAML ABI。
MCT1 使用 40 B 头、128 条 72 B 记录，每条保存 CPU IRQ 入口、counter 读取前后周期、
TSCC、RX 硬件锁存时间、当前 counter、名义波特率、读取前 FIFO 状态和中断标志。
记录首个匹配反馈，采用 revision/valid 保护；JTAG 仅在主站退出后导出。

MCAN TSCC.TCP=16：1 Mbps 时 16 us/tick，16-bit counter 在 1.048576 s 后回绕。
SDK 寄存器定义明确使用 CAN bit times；配置结构体中的 AHB 注释不能作为换算依据。
RX 硬件时间戳为 SOF，因此“锁存→读取”的差值包含报文接收时间，不能直接称为纯 IRQ 延迟。
CPU 计时和 MCAN counter 不做绝对时刻相减；主机解析器用相邻采样检查 counter 速率，
16-bit 差值只能在一个回绕范围内解释，无法排除更长延迟。

## 构建与测试

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_mcan_timing -- \
  -DDUST_ADAPTER_CAN_CONFIG=$PWD/config/can_bench_dm_rtt.yaml \
  -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_HPM_MCAN_RX_TIMING=y \
  -DCONFIG_DUST_ECAT_ESC_DMA=n
```

接收线程优先级 0、主线程 1，沿用 CAN3 ID3 失能白名单。主机仍使用原 125 us/64 样本工具，
本轮只添加观测，不同时修改 FIFO 排空、IRQ 优先级或 CAN 协议，以免混淆原因。
完整原生回归及诊断采样/sidecar 序号匹配、重复与重置测试通过。实板编译 RAM 244144 B，
包含有界诊断 RAM、线程栈和堆，不代表运行期最大栈水位已测量。

主站退出后，使用匹配 ELF 分别导出 `dust_ecat_trace` 与 `dust_mcan_timing`，
通过 `tools/dump_ethercat_rtt_trace.py --symbol dust_mcan_timing` 选择第二份 RAM。
在主机先运行 `tools/ethercat_rtt_decode.py` 生成分段 CSV，随后运行
`tools/ethercat_mcan_decode.py MCAN_RAM.bin STAGES.csv OUTPUT.csv`。

若 MCAN 硬件 age 本身接近数毫秒，检查 FIFO/IRQ 交付链路；若 age 仅约一个帧长，
而 CAN 提交→反馈仍约 6 ms，则应沿 TX 线上时间、总线与实际应答设备继续定位，
不能归因于接收线程队列等待。中断入口到回调的 CPU 时长可进一步区分驱动内处理。

首轮新增观测实测尚待执行；不把该诊断补丁称为 6 ms 修复。
