# DM 失能台架时间戳

默认关闭 `CONFIG_DUST_ECAT_RTT_TRACE`。开启后仅追踪 CAN3 ID3、8 字节 FF FF FF FF FF FF FF FD，反馈 ID13，每次只有一条在途命令。不改变 PDO、CAN 白名单、控制协议或业务看门狗时钟。

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_trace -- \
 -DDUST_ADAPTER_CAN_CONFIG="$PWD/config/can_bench_dm_rtt.yaml" \
 -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_BUILD_OUTPUT_BIN=y
bash scripts/test_ethercat_rtt_trace.sh
```

RAM 符号 `dust_ecat_trace` 含 40 字节头和 128 条 168 字节记录，停止录入后不覆盖；溢出单独计数。阶段枚举和布局见 `apps/ethercat_slave/src/rtt_trace.{h,cpp}`。使用 `k_cycle_get_64()`，时钟 Hz 写入头中，当前 24 MHz。IRQ 中只写内存，不打印、不动态分配。首轮建议只采 64 条命令。

16 个阶段：ESC SM2 读取前后、Rx 映射完成、协议处理前后、IO 接纳、CAN 入队、MCAN 提交前后、TX 完成回调、CAN 反馈回调、反馈捕获、发布完成、SSC 输入映射、ESC SM3 写入前后。发布仅在匹配 CAN 序号被捕获后记录。每个阶段只保留首次时间。

主站测试结束并释放 master 后运行；导出前不要复位，测试中不要 halt：

```bash
python3 tools/dump_ethercat_rtt_trace.py \
 build/dm_rtt_trace/zephyr/zephyr.elf build/dm-rtt-trace.bin \
 --nm /path/to/riscv64-zephyr-elf-nm \
 --openocd /path/to/openocd \
 --scripts ../sdk_env/hpm_sdk/boards/openocd
```

脚本按匹配 ELF 的符号查询地址和大小，通过 JTAG 暂停、导出、恢复 CPU，不复位。主机 `dust-control/tools/ethercat_rtt_decode.py` 按 boot/session/batch 关联并验证 CAN 序号，生成每段耗时。两个时钟未同步，不能跨时钟直接相减。CAN 完成回调、接收回调均为软件观察时刻，并非总线电平边沿。DM 不回显命令批次，仍需空闲检查和单在途约束。

2026-10-28：64 条 125 µs 台架命令全部成功，16 点完整、无溢出/驱动错误/序号错配。trace 固件 RAM 227180 B（86.66%），ROM 169836 B。详细数据及分析随主机仓库 `docs/ETHERCAT_RTT_TRACE.md` 保存。

## Aligned word access (2026-10-28)

The EtherCAT application now enables `CONFIG_HPM_ESC_PDRAM_WORD_ACCESS=y` and
pins hpm_support with aligned, exact-span PDRAM word transfers. Set this symbol
to n to reproduce the original byte trace baseline. Register accesses and
unaligned/non-multiple lengths keep the original access path. This is a CPU
optimization; DMA is not enabled.

Final 64-command 125 us run: all 16 stages complete, no overflow, CAN errors,
sequence mismatch, timeout or WKC loss. ESC read/write means 14.013/49.343 us
(previous byte baseline 31.446/98.114 us). RTT mean 746.987 us, maximum
998.614 us. This sample count does not establish a hard 1 ms bound.
The intermediate word test ran 1000 commands; the bounded trace retained only
its first 128 and counted 872 overflowed recordings. Full host CSV and the
retained trace subset are saved in dust-control, alongside the final 64-run
comparison. Support-level contracts run through scripts/test_protocol.sh.
