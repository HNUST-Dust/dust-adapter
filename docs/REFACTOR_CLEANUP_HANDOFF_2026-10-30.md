# 当前选用版本交接：重构与清理

更新日期：2026-10-30。交接对象：负责 dust-adapter 及配套主站工具重构、清理的 agent。

**先保留可复现基线，再重构。当前选用的是已通过 2000 次负载测试的 FreeRun＋单线程＋CAN 反馈历史窗口实验版，不是已经完成正式主站兼容的发布版本。**

本文描述当前状态；早期 `FULL_PERIPHERAL_RTT_HANDOFF.md` 中“卡在 PREOP、StartOutput 从未调用、怀疑 SSC 三缓冲/看门狗”的结论已被后续证据修正，不能继续作为定位前提。

## 1. 当前选用的组合

| 项目 | 当前选择 |
| --- | --- |
| 从站固件 | Mac `dust-adapter/build/periph_can3_window`，已烧录 |
| 同步模式 | FreeRun；DC、SM 同步均关闭 |
| EtherCAT 周期 | 125 μs |
| 从站调度 | 单 SSC 主线程，priority 0；CAN/UART IRQ 入队，无 CAN RX 工作线程 |
| 发布优化 | `DUST_ECAT_INPUT_LATE_REFRESH=y`；保留已选用的 overwrite-clear 跳过优化 |
| CAN 反馈策略 | 实验性历史窗口，默认关闭的 `DUST_ECAT_CAN_FEEDBACK_WINDOW=y` 由独立配置启用 |
| 主站测试程序 | NUC `~/dust-ecat-bench/dust_ethercat_peripheral_can3_rtt`，按帧序号去重 |
| 主站测试调度 | FIFO40、CPU7、mlock1 |
| 网卡 | I225，PCI `0000:58:00.0`，驱动 ec_igc；三个 ASPM 控制均关闭 |
| 本轮范围 | CAN3 三个电机的失能/零力矩＋UART2 HI91 RX＋UART5 SBUS RX |
| 暂不接入 | CAN2：用户确认可能有硬件问题，要求跳过；板载 IMU：驱动/芯片及 DMA 资源问题 |
| PDO | Rx208 / Tx688 字节，signature `2616507966`；TX 预留6槽但仅发送3帧 |

用户此前决定选用 FreeRun，不通过改固定相位偏移优化，不在此阶段重排 PDO 字段。重构时不要顺手切回 DC、加固定接收偏移或缩减 PDO 来改变比较条件。

## 2. 已验证结果与边界

最新完整会话：`peripheral-can3-window-01`，2000 条命令、2000 个顺序配对 DM 样本，**超时0、WKC缺失0**。

| 指标 | 结果 |
| --- | --- |
| RTT 均值 / P50 | 1150.232 / 1123.417 μs |
| RTT P95 / P99 / 最大 | 1372.655 / 1373.558 / 1498.354 μs |
| HI91 | 20232 个有效帧，CRC错误0、块序号缺口0、设备时间不推进0 |
| SBUS | 1423 个结构有效帧，4帧报告 frame_lost，failsafe0，块序号缺口0 |
| CAN3 去重反馈 | 0x201=21337、0x10=3999、0x13=2000；计数含准备阶段 |

这是约20秒的当前负载验证，不是长期稳定性或硬实时证明。此前失败会话的部分成功样本有截断偏差，不能用它们与本轮均值相减来宣称延迟收益。本轮明确收益是完整会话不再发生快照漏读超时。

SBUS 的 frame_lost 是收到的接收机载荷标志，不等于 UART 丢块；不要把本轮描述为所有遥控帧完全正常。更早遥控器关闭时，SBUS 全带 failsafe；用户后来开启后标志清除。

原始记录：`docs/bench-results/2026-10-30-full-peripheral/can3-window-01/`，包含 `run.log`、`rtt.csv`、`uart.csv`、`uart-summary.json`、`can.csv`、`can-flow.json`。

## 3. 工程位置与实际版本

### Mac：固件与依赖

工作区 `/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws`；应用为其下 `dust-adapter`。从当前 Linux 工作区访问 Mac 可用 `mac zsh -lc '...'`，NUC 可经 Mac 执行 `ssh wbr-nuc '...'`。

| 仓库 | HEAD | 状态 |
| --- | --- | --- |
| dust-adapter | `dd5d789a2077215945423edb10ee42ad16e9f57c`，分支 `ethercat` | 有未提交及未跟踪改动，单独检出 HEAD 不能复现当前版本 |
| hpm_support | `d1e9fbfbdca40c3ecc965795917735cb92bd6fd2` | MCAN DC相位诊断钩子、PHY维护/诊断相关改动未提交 |
| zephyr | `8c6db8fa778cf90eb93e823e513a1ab97703711d` | `drivers/interrupt_controller/intc_plic.c` 有未提交改动 |
| sdk_env | `7b8cb8bb2f04f52353a6429bffb024f0a2867876` | 核对时无 tracked 改动；`hpm_sdk` 位于该仓库内，Git根目录见快照 manifest |

其他依赖按 `west.yml` 保留。SSC 使用应用 `external/ssc` 的 FreeRun 生成源，`DC_SUPPORTED=0`、`AL_EVENT_ENABLED=1`、`INTERRUPTS_SUPPORTED=1`。不要拿 `/Users/panpoming/Downloads/dust_hpm5e_dc` 的 DC 生成物覆盖它。

SSC 源中 `MIN_PD_CYCLE_TIME=0x7A120` 仍为原值；当前125μs FreeRun测试已通过。不能仅看到该静态值就未经验证重新生成 SSC 或改同步模式。

### Linux/NUC：正式工程与独立台架不是同一份代码

当前协作工作区 `/home/panpoming/dust-control`：分支 `dev/wbr_infantry_ethercat`，HEAD `7a7af0873824909b7b6797d46c70a5f8ee27b237`，有大量既有未提交实验和结果，不能整体清理。

NUC 的正式仓库 `/home/wbr-infantry/dust-control`：同名分支，HEAD `d6dd3ab9b70d16e0c8d716264f8a1d94cc5f4fb4`，本次核对工作树干净。**不要假设它与当前工作区同步，也不要用 git pull 覆盖独立台架。**

独立台架为 `/home/wbr-infantry/dust-ecat-bench`，不在正式仓库内。当前内核 `5.15.0-1114-realtime`，IgH 1.7 主站，链接 `/usr/local/lib/libethercat.so.1`。当前使用原驱动，没有加载先前 RX 时间戳实验模块。

## 4. 重构前的冻结快照

已保留一份当前源代码与固件的恢复依据，不需要先提交才能重构，但不要删除这些快照。

Mac 目录：

```text
/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/dust-adapter/build/bench-host/selected-version-2026-10-30/
```

包含匹配的 `zephyr.elf`、`zephyr.hex`、`.config`、`CMakeCache.txt`，各仓库 `git diff --binary HEAD` 补丁、固件未跟踪文件归档、源文件及 SSC 文件哈希、仓库状态和 HEAD manifest。

另外已将该目录完整归档到工程及build目录之外：`/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/handoff-archives/selected-version-2026-10-30.tar.gz`。重构需要清理build时，应先确认这份外部归档仍在且能展开，再保留原始ELF/HEX作对照。

构建产物的权威哈希：

```text
ELF e06c9ef273ca6521fb38a0c6df54786164e36bc3739a429676dd42f8cb60891e
HEX 2fec604cc8b989dfff1b3843f0e58852c362c08ddc09470ba26593a6a5a78ea4
```

当前工作区也保存了补丁、manifest、未跟踪文件归档和选用工具源副本：`docs/handoff/2026-10-30-selected-version-snapshot/`。其中 `control-workspace-tracked.patch` 还包含此前其他实验的既有改动，不应全部当作本轮窗口方案。

NUC 独立工具包：

```text
~/dust-ecat-bench/handoff-selected-2026-10-30.tar.gz
~/dust-ecat-bench/handoff-selected-2026-10-30.json
```

包包含实际二进制、C++源、YAML、probe、`pdo_config.cpp`、编译用 `include` 和 ASPM 安装源/已安装配置。其 SHA256 为 `80a75f0af87332769e84c025b8d18e2e609c41f2f36ed943d7992807e7049ff5`。

关键工具二进制 SHA256：`613485080b4e8ea5c2649112fe6737137d05d9d95f4237781c31862a5a692f93`。其他文件哈希见 `nuc-manifest.json`。

恢复时在独立目录按对应 HEAD 准备依赖，先 `git apply --check`，再应用各自补丁并恢复未跟踪文件；SSC 文件必须与 manifest 匹配。当前已有工作树不要执行 `git reset --hard` / `git clean` 来“恢复”。重构后的二进制哈希通常会变；原始固件只用于回滚和对照。

## 5. 当前外设与安全行为

| 通道 | 发送 | 接收 |
| --- | --- | --- |
| CAN3，1Mbps | 0x200：8字节0；0x000、0x003：FF FF FF FF FF FF FF FD | 0x201、0x010、0x013；RTT目标为0x003→0x013 |
| UART2 | 不发送 | HI91，921600波特，128字节PDO接收容量 |
| UART5 | 不发送 | SBUS，100000/8E2，32字节PDO接收容量 |

固件配置为 `config/can_bench_can3_peripheral.yaml`：移除 CAN2 的配置，保留 CAN3 三地址白名单及启动/停止保护帧，CAN0/1为空设备列表。默认 `config/can_devices.yaml` 仍含 CAN2，不可在重构时误用默认构建烧录当前台架。

台架会 Arm **网关业务会话**以允许转发，不发送电机使能帧。不要把它与禁Arm的启动检查工具混淆。所有当前电机测试仅失能/零力矩；不向其他地址、CAN2 或 UART 发业务命令。

保持白名单、保护帧、Stop/保护状态、源租约、序号/会话校验、8ms台架应答截止期及严格WKC失败判据。不要为了跑通测试删除保护、自动重试超时命令或放宽配对窗口。

SPI3/IMU 代码目前仍编译：`CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW=y`，主站 `onboard_imu:false`。板上 ICM-42688P WHO_AM_I=0x47，现驱动期望0x6A；SPI3还缺可用异步DMA。不能将本次通过描述为IMU已工作。IMU相关 `CMakeLists.txt`、overlay、prj.conf、io_backend 改动是用户先前待完成工作，不能直接删除当作诊断垃圾。

不要按“目前只用两路UART”就直接删其他UART/DMA节点；先核对驱动资源分配、已安装保护策略及以后IMU资源规划。

## 6. 三个已经定位的问题

1. **CAN2阻塞OP**：CAN2故障标志在Ready阶段触发Transport保护，StartOutput实际被调用但因Protected返回0x25。旧 `dust_config_diag` 当时没有置StartOutput bit2，造成误判。当前已用独立profile排除CAN2，未放宽保护。
2. **ASPM导致125μs WKC失败**：相同OTHER调度下，开启ASPM可重现WKC持续0；关闭后1000个OP周期缺失0。FIFO不是该启动检查通过的必要条件，但性能测量继续用FIFO。ASPM三个开关目前成组关闭，尚未分离各子状态贡献。
3. **CAN反馈快照覆盖**：`can3-diag-01` 的第317条命令已被ACK，DM帧seq75692已收到/入队/出队，并映射为publication19216；122μs后被新关节反馈替换为19217。主站记录只有19215及19217，没有该DM。证据见对应 `README.md`、`can-flow.json`、`can.csv`。

当前窗口实验保留配置容量内、未超过20ms的近期CAN事件；满时淘汰最旧帧，不按CAN ID合并、不增加PDO长度。8槽窗口在本轮末尾记录中的映射可见时间中位数约6.1ms。它不是有确认队列，不能保证任意负载每帧必达。

## 7. 正式化前的兼容性工作：重构不能掩盖

**窗口固件与现有正式主站不兼容。** 两份正式仓库的 `src/hardware/ethercat/session.cpp` 当前会对跨批次重复/旧frame sequence报 `EtherCAT CAN event reordered`；本轮只有独立台架增加去重。

如果后续要正式采用窗口方案，需要单独完成并测试：

- 明确能力或模式协商，使旧主站不会默默混用新窗口语义；仅重构目录不算完成此项。
- 严格区分合法的已消费前缀重叠与真正乱序；不能直接把台架的“所有非新序号都continue”复制进正式解析器。
- 验证帧顺序、重复内容、序号回绕、重启/新session、过期、容量淘汰及真实缺口，并保证新帧只解码一次。
- 重新定义淘汰/丢失计数：窗口沿用 `drops`，正常淘汰已被读取的旧帧也会加计数。正式主站目前据drops变化重置电机角度unwrap；直接混配会误复位。

这部分属于功能/协议演进，宜与纯重构分开评审。当前默认开关保持n，窗口通过 `config/can_window_diag.conf` 独立启用。

## 8. ASPM持久配置：不要在清理时移除

用户明确授权关闭专用网卡节能。已安装：

```text
/usr/local/sbin/dust-ethercat-disable-aspm
/etc/systemd/system/ethercat.service.d/60-disable-aspm.conf
```

drop-in 使用 `ExecStartPost` 在 EtherCAT 服务启动后对 Intel `8086:15f2` / `0000:58:00.0` 设置 `l1_aspm=0`、`l1_1_aspm=0`、`l1_1_pcipm=0`；服务已开机启用，当前三个值已核对为0。没有关闭全系统节能，没有替换内核/网卡驱动。跨系统重启效果尚未实测。

源码位于当前工作区 `tools/system/ethercat-aspm/`，NUC部署为 `~/dust-ecat-bench/ethercat-aspm/`。旧实验脚本若保存/恢复ASPM，应保存当前0并恢复0，不能写死恢复1。

## 9. 未提交改动的清理分类

| 类别 | 位置 | 处理原则 |
| --- | --- | --- |
| 当前窗口行为 | Kconfig、`config/can_window_diag.conf`、io_backend、对应原生断言 | 保留独立开关及有界/过期语义；正式兼容未完成前不设为默认 |
| 当前CAN3硬件范围 | `config/can_bench_can3_peripheral.yaml` | 保留可复现profile，不合并进默认全设备配置 |
| 低开销发布/PHY维护优化 | prj.conf已有选项及hpm_support PHY相关代码 | 核对启用条件与快照后再拆分，不把有行为影响的代码当日志删除 |
| 临时ESM/CAN链路诊断 | `esm_diag.c/.h`、`dust_esm_diag`、`dust_can_flow_diag`；Core/Gateway/ESC/IO中的调用 | 结果及原始固件已归档，可按独立诊断模块隔离；确认回归后再去除发布构建中的调用 |
| 临时配置printk/调用标志 | gateway中的 `dust_config_diag`、APPL回调printk/bit标志及前置声明 | 可以整体清理，但先核查全部引用，不仅删定义；保留原有错误保护和状态机行为 |
| 旧DC/worker/相位/RX时间戳实验 | config、tools/experiments、hpm_support MCAN相位钩子、Zephyr PLIC改动等 | 分开归档；当前FreeRun不代表可以未经检查删除整个实验框架或依赖改动 |
| IMU接入 | CMakeLists、overlay、prj.conf、io_backend | 明确为未完成功能；保留或隔离，不冒充本轮已验证功能，也不擅自删掉 |
| 实验结果与工具 | docs/bench-results、独立bench、当前工作区tools/tests既有改动 | 不做整目录清除；有选用版本和证据的索引，后续可单独制定归档策略 |

本轮没有commit、push或merge；以上dirty状态是交接事实，快照保存的是混合工作树，不应把它们不加区分地一次提交为“清理”。

## 10. 构建、烧录与测试复现

Mac：

```bash
cd /Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/dust-adapter
export PATH=/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5
export CCACHE_DIR=/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/.ccache
west build -b dust-hpm5e31 -d build/periph_can3_window -- \
  -DEXTRA_CONF_FILE=config/can_window_diag.conf \
  -DDUST_ADAPTER_CAN_CONFIG=/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/dust-adapter/config/can_bench_can3_peripheral.yaml
west flash --skip-rebuild -d build/periph_can3_window
```

重构用新构建目录，不要覆盖已保存的基线。当前ROM191316B、RAM217640B，RAM约83%；清理后的资源变化要记录。

NUC台架源由当前工作区 `tools/experiments/peripheral_can3/bench.cpp` 部署为 `ethercat_peripheral_can3_rtt.cpp`，`probe.sh`部署为 `ethercat_peripheral_can3_probe.sh`。编译：

```bash
cd ~/dust-ecat-bench
g++ -std=c++20 -O2 -Wall -Wextra -I include \
  ethercat_peripheral_can3_rtt.cpp pdo_config.cpp \
  -o dust_ethercat_peripheral_can3_rtt -lethercat -lyaml-cpp -pthread
```

每次独立实机会话先在主站非活动状态复位网关，等待重新发现PREOP；上一轮Stop后Core会Protected，不复位可能导致SDO配置被拒。现有Mac `build/reset_periph.py` 只复位不重新烧录，可作为现场辅助，不要当成已版本管理的部署接口。

```bash
sudo bash ~/dust-ecat-bench/ethercat_peripheral_can3_probe.sh 2000 <新结果标签>
```

标签不得覆盖旧结果。sudo密码必须由用户交互输入；不能把“脚本已部署”当作“测试已执行”。OTHER可做短功能检查，但本轮曾在189个样本后发生严格截止期缺失，不拿OTHER结果宣称性能。

台架通过UART累计ACK获取后续数据；不能删除ACK然后仅凭反复读取同一块数据判传感器健康。UART采样只写预留内存，结束后才落盘；40000块满时明确失败。离线分析：

```bash
python3 tools/experiments/peripheral_can3/analyze_uart.py <结果.csv.uart.csv>
```

### 原生回归

固件脚本 `scripts/test_ethercat_io.sh`、`scripts/test_ethercat_ssc.sh` 已通过；窗口变体另给首个单线程IO目标增加 `-DCONFIG_DUST_ECAT_CAN_FEEDBACK_WINDOW=1`，验证保留、容量淘汰和过期。原生外设边界是mock，不能代替实机。

Mac自带Bash3的 `nounset` 与空数组存在兼容问题；本轮用内存脚本关闭nounset、保留errexit/pipefail/断言/sanitizer，并显式设置root后运行，未因此修改原仓库测试脚本。相关运行日志为固件 `build/io-baseline-window.log`、`build/io-window-window.log`、`build/test_ethercat_ssc-flow.log`。不要把Bash兼容错误误判为产品逻辑失败，亦不要吞掉真实测试返回码。

## 11. 故障现场与结束状态

**主站活动期间禁止JTAG halt。** 必须等待bench退出、确认 `ethercat master` 为 `Active: no`，再halt/dump并保证resume；读完现场前不要复位/烧录。解析符号必须用该次运行匹配的ELF，不能将旧 `periph_can3_diag` 地址用于窗口固件。

当前选用版本的匹配现场保存在Mac `build/bench-host/peripheral-can3-window-01`。最后已导出并恢复CPU运行，没有再次复位；当前主站Idle/非活动、从站PREOP，Core在停止后的状态不是运行期故障证据。下一次独立测试需重新复位。

### 建议接手顺序

1. 阅读本文、最新通过结果及冻结manifest，先确认所处的是Mac固件工程还是Linux/NUC主站工程。
2. 保留原始选用HEX/ELF、独立工具包及dirty补丁，再把代码按功能、实验、诊断、未完成IMU分开整理。
3. 先完成不改变行为的重构与诊断隔离；保持独立profile和窗口默认关闭。
4. 以独立任务处理窗口与正式主站的去重/兼容/丢失计数，不把实验通过等同于生产接口已完成。
5. 原生回归后，在相同ASPM/FIFO/125μs/PDO/外设条件下做至少同规格2000次及UART离线校验；失败保留现场。一次烧录可包含多项验证，但独立会话之间仍需复位。

本交接文档和快照创建没有重启网关、服务或操作系统，没有更换当前选用固件，也没有启动新的电机测试。
