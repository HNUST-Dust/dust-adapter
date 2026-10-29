# DM 失能台架时间戳

默认关闭 `CONFIG_DUST_ECAT_RTT_TRACE`。开启后仅追踪 CAN3 ID3、8 字节 FF FF FF FF FF FF FF FD，反馈 ID13，每次只有一条在途命令。不改变 PDO、CAN 白名单、控制协议或业务看门狗时钟。

```bash
west build -b dust-hpm5e31 -d build/dm_rtt_trace -- \
 -DDUST_ADAPTER_CAN_CONFIG="$PWD/config/can_bench_dm_rtt.yaml" \
 -DCONFIG_DUST_ECAT_RTT_TRACE=y -DCONFIG_BUILD_OUTPUT_BIN=y
bash scripts/test_ethercat_rtt_trace.sh
```

RAM 符号 `dust_ecat_trace` 含 40 字节头和 128 条 168 字节记录，停止录入后不覆盖；溢出单独计数。阶段枚举和布局见 `src/ethercat/slave/rtt_trace.{h,cpp}`。使用 `k_cycle_get_64()`，时钟 Hz 写入头中，当前 24 MHz。IRQ 中只写内存，不打印、不动态分配。首轮建议只采 64 条命令。

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

DMA 实验、错误处理及对照结果见 [ETHERCAT_ESC_DMA.md](ETHERCAT_ESC_DMA.md)。默认保持 CPU word 路径。

CAN 独立采集、UART 保持与时间戳 ABI v2 见 [ETHERCAT_CAN_RX_FAST_PATH.md](ETHERCAT_CAN_RX_FAST_PATH.md)。


## DTR1 v4 worker execution markers (2026-10-29)

Record size is 208 bytes (`<IIQIIIIii21Q>`); the 40-byte header and 128-record
capacity are unchanged. All v3 stages retain their indices. Append stages 19
WORKER_WAKE and 20 WORKER_LOCKED. This adds 2048 bytes of trace RAM.

WORKER_WAKE records the first trace call after `k_sem_take` returns;
WORKER_LOCKED records the first trace call after acquiring the capture mutex.
Both are accepted only after the matching feedback has been enqueued and before
it is dequeued. They are optional: the worker may already be executing when an
IRQ queues feedback, including a wake marker before enqueue followed by a lock
marker after enqueue. Missing markers must not be interpreted as zero latency.
First accepted markers are retained; stale wakeups before enqueue and passes
after dequeue cannot overwrite them. Existing CAN sequence matching and the
CAN_SNAPSHOT requirement for PUBLISH remain unchanged.

Decode enqueue→wake, wake→locked, and locked→dequeue when both endpoints exist.
These measure execution boundaries, not pure scheduler or mutex costs: they
include instrumentation, interrupt preemption and per-port queue scanning.
The first interval includes semaphore signaling and remaining ISR execution.
No IRQ-exit hook or SSC phase attribution is added in this revision; those need
separate observation before assigning the remaining waits to a cause.

The host decoder accepts v1–v4; v4 completeness requires the original 19 stages,
with worker markers exported and checked for order only when present. This
revision changes observation only, not polling, priorities, PDO or IO policy.
Native trace/IO and host decoder tests pass. Target build is checked separately;
no board measurement or latency improvement is implied by these changes.


## DTR1 v5 CAN3 scan boundary (2026-10-29)

Append optional stage 21 CAN3_SCAN at the start of the CAN3 iteration of
DrainCanReceive, before retained snapshot filtering and message queue reads.
Record size is 216 bytes (`<IIQIIIIii22Q>`); header and capacity unchanged.
The same enqueue-before / dequeue-after acceptance window used for worker
markers applies. Host v1–v4 decoding is retained; the archived worker-v4-01
run replays to byte-identical CSV. Native trace and IO contracts, 8 host decode
contracts and the HPM5E31 trace build pass.

When endpoints exist, WORKER_LOCKED→CAN3_SCAN covers worker bookkeeping and
CAN0–2 scanning; CAN3_SCAN→CAN_DEQUEUED covers retained CAN3 snapshot filtering,
queue read and trace entry. Both include instrumentation and preemption. They
are optional observation markers, not pure CPU service times, and can be
missing if an IRQ arrives during an already executing pass. No receive policy,
priorities, polling, or SSC ownership changes are made.

Source inspection also shows empty bus iterations still invoke nonblocking
k_msgq_get and Now() for expiry. Zephyr's queue read takes a spinlock even for
an empty queue, and k_uptime_ticks takes the timeout spinlock. This provides
concrete candidates for later measurement, not evidence of their individual
cost. In particular, avoid skipping disabled ports without considering queued
frames, drops, configuration transitions and stale feedback cleanup.


## DTR1 v6 SSC snapshot-wait attribution (2026-10-29)

Record 256 bytes: `<IIQIIIIii26QII>`, 40-byte header and capacity 128 unchanged.
Stages 0–21 retain v5 meanings. Append optional WORKER_WAIT (22), SSC_SERVICE
(23), SNAPSHOT_BEGIN (24), SNAPSHOT_LOCKED (25). These are first calls after
matched CAPTURE and before matched CAN_SNAPSHOT. Missing markers are not zero
latency: the owner may already be inside a service/capture call when preempted.
WORKER_WAIT marks the next semaphore-take entry after the capture pass releases
its cache mutex, not proof that the worker actually blocked (a token can exist).
SSC_SERVICE is ServiceCanReceive entry; SNAPSHOT_BEGIN is CaptureCanFeedback
entry; SNAPSHOT_LOCKED follows acquisition of the snapshot mutex. Existing
CAN sequence matching and SNAPSHOT-before-PUBLISH eligibility are preserved.

Two uint32 fields follow the 26 timestamps: capture_phase and snapshot_phase.
They latch the owner's coarse phase at the first matching CAPTURE and SNAPSHOT.
Phase enum IDs 0–15: unknown, outer, ssc, maintenance, wait, poll_tick, poll_io,
poll_can, poll_publish, app_tick, app_can, app_process, app_io, app_publish,
esc_read, esc_write. Gateway/application callbacks restore the caller phase on
all exits; ESC block transfers >=64 bytes temporarily override it. Small ESC
register operations are included in the enclosing phase. No SSC vendor sources
are modified. Owner phase is an observation label, not a scheduler state; a
phase at capture does not prove all later waiting is spent in that phase.

Interpret capture→worker_wait as worker tail + instrumentation/preemption;
capture→ssc_service as delay until the next CAN service checkpoint;
capture→snapshot_begin, begin→locked and locked→snapshot split snapshot request,
mutex and expiry/copy work. Check markers individually: not all events form one
mandatory linear chain. In particular a main-thread checkpoint is not an exact
scheduler resume hook. No pure context-switch cost or continuous phase duration
is inferred. Main checkpoints/phase changes add trace overhead; use this build
for localization, not a direct performance comparison with v5 / trace-off.

Host decoder accepts v1–v6 and retains the original mandatory stage mask. Nine
host decoder tests pass, as do native trace, IO and SSC integration contracts;
v5 measured replay is byte-identical. HPM5E31 trace build passes (243936 B RAM).
Empty-cache optimization from worker-v5-opt-01 is retained unchanged.


## DTR1 v7 pending write and SSC loop tail (2026-10-29)

Record 280 bytes `<IIQIIIIii29QII>`, header/capacity unchanged. Append optional
PENDING_WRITE_END (26), SSC_RETURN (27), POLL_ENTRY (28); phase fields remain at
the end of the record. Same matched CAPTURE-before / SNAPSHOT-after acceptance
window as v6 wait markers. These markers never replace sequence-matched ESC_END.
PENDING_WRITE_END marks the first completion of an input write at 0x1d00,
length >=64, while the feedback still awaits a snapshot. It is not necessarily
the write executing at capture; require capture_phase=esc_write for that group.
SSC_RETURN follows MainLoop return, before restoring the outer phase. POLL_ENTRY
precedes gateway poll bookkeeping. All are optional and preserve their first
accepted value; the matching feedback's later ESC write cannot fill a missing
pending marker after snapshot. No recursive poll/SSC calls are introduced.

For records containing the chain, inspect capture→pending_write_end,
pending_write_end→ssc_return, ssc_return→poll_entry, poll_entry→ssc_service.
These include worker execution, preemption and instrumentation, not pure CPU
service times. Records captured in another phase may not traverse every point.
No mandatory completion rule is added. Native trace/SSC and ten host decode
tests pass; v6 measured replay is byte-identical; target trace build passes.
