# dust-adapter / dust-control 重构交接

更新日期：2026-10-23。本文供下一位 agent 直接接手。当前是成对重构的功能分支，尚未完成新版本硬件验收，不能当作已部署正式版。

## 1. 用户目标和已确认约束

- MCU 负责 USB ↔ CAN/UART 及板载 IMU 原始数据转发；NUC 运行 chassis_controller、传感器解析、电机协议和控制算法。
- 最新决定：MCU 应通过配置文件知道各路 CAN 设备 ID 和断链时的保护帧。不是完全无设备约束的透明转发器。
- 配置采用编译期 YAML；不引入 FatFs，不在 MCU 运行时读文件，不运行时配置 UART 波特率等参数。
- MCU 不解码电机位置/力矩、不运行底盘算法；保护数据是配置中的原始 CAN 字节。
- USB 固定使用 High-Speed Bulk；控制命令合包，默认左右六帧放在一次 128 字节 OUT 中。每路 CAN 的应用命令顺序 W/B/D。
- 指令和反馈解耦，反馈持续流式上报，不等待六个设备收齐，不把 ACK 当电机 RTT。
- 保留原 MCU 已验证的遥控使能/失能策略、方向、零点和算法约定，避免再做冗余的实物方向确认。
- 可读性、模块职责和可维护性是明确要求。不要继续堆叠临时测试/诊断程序。
- main 约定为 USB 正式版；EtherCAT 独立分支，仅保留原有从站基础，不在本次扩展成完整网关。v0.1 历史标签不得移动。
- 本轮没有授权自动刷板、使能电机或启动机器人。代码构建/单元测试不能等同硬件验收。

## 2. 正确目录、仓库和分支

| 项目 | 位置 / 状态 |
|---|---|
| MCU west 工作区 | `/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws` |
| MCU Git 仓库 | 上述目录的 `dust-adapter/`，不是工作区根目录 |
| MCU remote | https://github.com/HNUST-Dust/dust-adapter.git |
| MCU 当前分支 | `feature/generic-usb-adapter`，代码 HEAD `a685ebe` |
| MCU 草稿 PR | https://github.com/HNUST-Dust/dust-adapter/pull/4 |
| 上位机 Orb Ubuntu 路径 | `/home/panpoming/dust-control`，即 `~/dust-control` |
| 上位机 Mac 挂载路径 | `/Users/panpoming/OrbStack/ubuntu/home/panpoming/dust-control` |
| 上位机 remote | https://github.com/HNUST-Dust/dust-control.git |
| 上位机当前分支 | `feature/generic-usb-adapter`，代码 HEAD `c540a55` |
| 上位机草稿 PR | https://github.com/HNUST-Dust/dust-control/pull/4 |

两个功能分支都已经推送；不要未经验收直接合入正式分支。

**工作区纠正记录：**前一位 agent 曾误在旧目录 `/Users/panpoming/Documents/zephyr_projects/wbr_control_ws/dust-adapter` 开发和构建。代码已推送，正确工作区的仓库现已切到同一功能分支和 `a685ebe`。之后只能以 `dust-adapter-ws/dust-adapter` 为 MCU 主工作目录。旧目录和 `/tmp` 构建目录仅供追溯，不能继续当主工程。

不要误改旧的 Orb `~/wbr_control_companion`；那是另一份旧工程，可能有其他 agent 的改动。NUC 历史目录是 `~/wbr_realtime_lab/src/wbr_control_companion`，不能未经核对将其视为已迁移的 dust-control。

## 3. 第一件事：校准新工作区依赖

正确工作区的 `hpm_support` 已同步到 `west.yml` 固定的
`d85eefba7aacdae79c9beaaf7a4378f8cd5f9572`，工作区干净。该提交同时把此前只在
fix 分支上的 UART/CAN/PLIC 修复带入 `main`（`main` 已快进到该提交）。

```bash
cd ~/Documents/zephyr_projects/dust-adapter-ws
# 激活本机可用的 west/Python 环境，先核对所有仓库的未提交改动
west update
west list
cd dust-adapter
bash scripts/test_protocol.sh
bash scripts/build.sh usb
```

若只更新 HPM 依赖可用 `west update hpm_support`，但仍需检查其他依赖是否与清单一致。先保留任何已有本地改动，不要 reset/clean 覆盖。

固定依赖：

- Zephyr：`8c6db8fa778cf90eb93e823e513a1ab97703711d`。
- HNUST-Dust/hpm_support：`d85eefba7aacdae79c9beaaf7a4378f8cd5f9572`，包含 UART 正式修复、UART10 板级空闲检测和 CAN RTR 过滤匹配修复，不需手工补丁。
- sdk_env：`7b8cb8bb2f04f52353a6429bffb024f0a2867876`。
- CherryUSB：`50c7e9085801e24d2d0991d0c4c1609fdbcfb90a`。
- segger：`b011c45b585e097d95d9cf93edf4f2e01588d3cd`。

历史可用 west：`/Users/panpoming/Documents/zephyr_projects/.venv/bin/west`。CI 使用 Zephyr SDK 0.16.8，历史本机验证使用 0.16.5。SDK 实际目录按新工作区安装情况确认，不要照搬旧缓存的绝对路径。

## 4. 已完成：配置驱动的 CAN 与保护策略

两仓库均有 `config/can_devices.yaml` 和 `tools/generate_can_profile.py`，当前内容一致。

- 编译时生成 `can_profile_generated.hpp`，不提交生成文件、不在实时循环解析 YAML。
- `DUST_ADAPTER_CAN_CONFIG` 可指定其他 YAML；上下位机可明确指定同一个文件。
- 配置总线 channel/bitrate、设备名、command_id、feedback_ids、标准/扩展格式、RTR、保护帧和最大租约。
- 精确 RX 过滤器匹配 ID/IDE/RTR；未声明反馈不转发。
- 所有主机发送记录先做白名单校验，拒绝未知总线/ID/格式，避免一包只执行部分记录。
- 同一组控 ID 可共享，完全一致的保护帧去重，冲突配置在生成阶段报错。
- 最多 16 个 RX 过滤器/总线、32 个保护帧；保护表不受单 USB 包最多六条记录限制。
- 默认最大租约 20000 µs，主机可缩短但不能延长。默认启动时也提交保护帧。
- `ConfigureSafety` 的空表表示使用 MCU 默认保护，不能删除默认表；旧客户端只允许确认完整相同的 CAN 表。
- 可选会话 UART 保护写只能补充，不能替换 MCU CAN 保护表。
- 显式 Stop、租约过期、会话替换、USB 丢失/传输故障会走停止/失效路径；清除待处理业务写，提交保护帧。
- 已经进入硬件发送的 CAN 帧无法撤回。保护只提交一组，不无限重复；没有物理停机保证。
- Ready 上报保护能力、数量、最大租约和 CRC32。签名覆盖保护帧和租约，**不是完整反馈路由哈希，也不是认证机制**。
- 上位机比较编译期签名，并核对运行时 motor_map 的命令/反馈地址在本机编译配置中，再允许 Arm。Arm 仅许可转发，不等于电机 Enter。

默认六设备（CAN2/3 都是 1 Mbps）：

| CAN | 设备 | 命令 ID | 反馈 ID | 保护 |
|---|---|---|---|---|
| 3 | left_wheel | 0x200 | 0x201 | 八字节零电流 |
| 3 | left_joint_b | 0x000 | 0x010 | FF FF FF FF FF FF FF FD |
| 3 | left_joint_d | 0x003 | 0x013 | FF FF FF FF FF FF FF FD |
| 2 | right_wheel | 0x200 | 0x201 | 八字节零电流 |
| 2 | right_joint_b | 0x001 | 0x011 | FF FF FF FF FF FF FF FD |
| 2 | right_joint_d | 0x002 | 0x012 | FF FF FF FF FF FF FF FD |

`protection_frame: null` 表示该设备明确不发保护写；RX-only 设备可省略 command_id，但不能声明保护写。完整格式见 MCU `config/README.md`。

## 5. USB v2 架构和代码入口

协议已从旧机器人专用 v1 改为 v2，不兼容旧固件/上位机，不做宽松回退。

- OUT 固定 128 字节，包括握手/Stop；IN 固定 64 字节。
- CAN OUT 原始记录 16 字节，一包最多六条；CAN IN 记录 24 字节，一包最多两条，包含 MCU 接收时间戳和每总线序号。
- UART Write 最多 96 字节；接收为字节流块，块边界不是传感器帧边界。
- 板载 IMU 保持原始数据转发。
- Hello → Ready → 默认保护/租约配置 → Result → Arm → Result；完成后才进入业务。
- 主机在 Hello 前预提交四个 IN，避免启动阶段产生数毫秒的假长尾。
- USB 独立事件线程；业务控制线程是唯一业务发送者，不用独立心跳掩盖控制线程卡死。
- 正常命令不逐包回成功 ACK；错误会使主机链路失败；不自动重使能。
- MCU CAN 以 FIFO 转发，不再以六电机 latest-value 槽位实现通用层。应用内去重和 WBD 排序在主机 MotorCommands。
- CAN 队列数据保留采集时会话；USB 断开/reset 的 epoch 与队列处理避免旧数据被新会话重标。
- UART TX 是异步 DMA 队列；DMA 完成后仍等待发送器空，避免 FIFO 尾部被重置。

MCU 入口：

- `src/main.cpp`：入口；`src/gateway_runtime.cpp`：握手、租约、Arm、收发路由、停止保护。
- `src/protocol.hpp`：便携 wire/session 协议。
- `src/client.hpp`：主机包构造函数。
- `src/drivers/firmware_can_profile.hpp`：白名单与防保护覆盖策略。
- `src/drivers/can_dispatch.cpp`：CAN 过滤、FIFO、三帧硬件批次。
- `src/drivers/usb_session.cpp`：USB 会话、传输、断连。
- `src/peripherals.cpp`：UART 外设及转发。
- `src/drivers/onboard_imu.cpp`：板载 IMU。

主机入口：

- `modules/usb/usb_link.cpp`：libusb、握手、租约、配置核对、Arm、生命周期。
- `modules/usb/motor_commands.hpp`：每周期命令合并与 WBD 顺序。
- `modules/usb/sensor_decoder.cpp` / `sensor_store.hpp`：原始流解析与状态发布。
- `modules/usb/hi91_time_guard.hpp`：HI91 时间回退/重启处理。
- `modules/chassis/`：底盘算法；`modules/runtime/`：实时循环。
- `modules/config/` 和 `config/`：启动配置；`modules/telemetry/`：可视化数据。
- `include/protocols/`、`modules/protocols/`：上位机电机/遥控/传感器协议，应保留。

上下位机的 `src/protocol.hpp`、client.hpp、YAML 和生成脚本目前手工保持字节一致。后续可统一来源或加入自动一致性检查，避免漂移，不要只改单侧 wire 定义。

## 6. UART 已定位问题与必须保留的修复

- UART10/SBUS：100000 8E2；UART2/HI91：921600 8N1；UART3：115200 8N1，参数固定。
- UART10 原先积攒到 128 字节，造成约 84 ms 更新间隔。DMA 清除 FIFO timeout 条件，单独启用 PLIC 或调整 FIFO 阈值不能可靠修复。
- 已利用板上反相 RX 同接 PC04/UART10 和 PB13/TRGM1_P03，经 TRGM1→GPTMR2 检测 1000 µs 无边沿，发布 DMA 已到达的部分区间。
- 保留 128 字节异步双缓冲、非缓存内存、offset/generation 边界约束；DMA 满仍负责切换。
- SBUS 实际更新约 14 ms，1000 µs 空闲门限不等于遥控 1 kHz。
- HI91 的长尾主要曾由 IN 预提交晚于 Hello 引起；必须保留先 prepost 再 Hello。
- HI91 断电重启会使设备时间回退；主机已有重建时间基准逻辑。
- 过去热插拔偶发引起 USB 断开，用户决定暂按硬件操作问题搁置；不能宣称已根治。

## 7. 已完成验证与证据边界

当前提交的 GitHub CI 都通过：

- MCU https://github.com/HNUST-Dust/dust-adapter/actions/runs/39830159867
- 主机 https://github.com/HNUST-Dust/dust-control/actions/runs/39830161233

本机离线验证：

- MCU dust-hpm6750 固件成功构建，ROM 87484 B、RAM 110668 B；详细哈希见 `docs/generic_build_verification.json`。
- 11 个 Python 配置生成测试通过；带 ASan/UBSan 的协议/会话与保护策略 C++ 合约测试通过。
- 单独 32 保护帧配置构建成功。
- Orb 主机 `build/generic-adapter` 8/8 CTest 通过，包括握手/拒绝/不匹配配置/重连及原有逻辑回归。
- 这些 MCU 本机构建在旧目录/临时目录完成，**正确 dust-adapter-ws 尚未重新构建**。
- 当前通用 v2 功能分支未刷板、未做硬件连机/延迟/停机验证。

主机离线验证命令（Orb Ubuntu）：

```bash
cd ~/dust-control
cmake -S . -B build/generic-adapter -G Ninja -DBUILD_TESTING=ON \
  -DDUST_ENABLE_FOXGLOVE=OFF -DDUST_BUILD_MPC=OFF -DDUST_BUILD_TOOLS=OFF
cmake --build build/generic-adapter --parallel 2
ctest --test-dir build/generic-adapter --output-on-failure
```

Mac 可用 `/usr/local/bin/orb -m ubuntu ...` 在虚拟机执行命令。不要在 Mac 对 Linux 构建目录直接编译。

历史旧正式链路已做过满负载零输出、kill/freeze、USB 拔插、遥控/HI91/左右电机反馈失联，以及实际底盘运行；用户两轮 30 秒未发现控制性能问题。但这些成绩不是当前重写后的 FIFO/v2 的验收结果。

## 8. 下一步推荐顺序

1. 在正确工作区核对分支和未提交改动，同步 manifest 依赖，重做固件构建/native 测试。
2. 审查两个草稿 PR 的协议边界、并发/生命周期、保护路径、实时循环分配/阻塞和代码可读性；优先修正确性问题，避免边改边测、无法归因。
3. 配对硬件验收先用停止态：UART/SBUS/HI91/IMU 流完整性、六路 CAN 映射和时间戳；不要使用旧 v1 主机。
4. 用台架 CAN 对端测试声明/未声明 ID、IDE/RTR、FIFO、溢出；UART TX 做跨 DMA 边界连续流/回环，尤其核对 UART10 TX 引脚条件。
5. MCU 独立供电，做主机退出/kill/freeze、USB 断开、会话替换、超时保护、配置不匹配拒绝、启动保护测试。分别确认软件提交、CAN 实际 TX 完成与设备停机。
6. 满负载 CAN/UART/IMU 并行重新测延迟和丢失；不要复用旧 p50/p99 结论。
7. 再做遥控失能/使能和实际控制，需用户操作确认和显式 enable-output。
8. 验收通过后再推广为正式 USB 分支/main；不要移动 v0.1 标签。EtherCAT 不在当前成对重构范围。

若继续重构，优先解决配置/协议共享源的维护、精简 main 的职责、错误/状态命名清晰度。Foxglove、ROS2 通讯和新算法属于上位机后续方向，不应塞回 MCU 通用层，也不应同时改变 wire 协议和控制算法而失去对照。

## 9. 维护交接注意

- 读每个仓库适用的 AGENTS.md 后再修改；尊重其他 agent 的现有未提交改动。
- 新增有价值的合约/验收即可，不恢复已撤掉的大量一次性压测和诊断产品代码。
- 当前依赖修复是正式 hpm_support 提交，不要恢复手工 patch 工作流。
- 本文两仓库各放一份 `docs/REFACTOR_HANDOFF_2026-10-23.md`；后续若只维护一份，请明确另一份的指向。

## UART 统一接收更新（2026-10-23）

三路 UART 已统一异步 DMA 128 字节双缓冲；UART2 循环 DMA、16 字节保护区
和轮询线程已移除。所有 RX 缓冲使用非缓存存储。UART10 已验证过的
TRGM1/GPTMR2 路径保留，现已下沉到 hpm_support 的 UART 驱动
（`drivers/serial/uart_hpmicro_line_idle.c`，开关
`CONFIG_UART_HPMICRO_RX_LINE_IDLE`，DT 节点 `uart10_idle_timer` 在
dust-hpm6750 板级 DTS 中）；其他板卡使用驱动默认空实现。板级 overlay 已迁入
boards/，旧构建缓存需 pristine。UART2/3 当前使用驱动原生超时，不能将
1000 us API 参数视为实际部分缓冲发布时限。本次改动尚未做硬件验收，原循环
DMA 的验收结论不适用。

## 2026-10-23 晚补记：正确工作区重建与提交

- 已在正确工作区把 `hpm_support` 从 `a31c069` 更新到 `335b77f`，随后又更新到
  `d85eefb`，各依赖与 `west.yml` 一致，工作区无未提交改动。
- 在该工作区执行 `scripts/test_protocol.sh`：11 个配置生成测试和两组带
  ASan/UBSan 的 C++ 合约测试通过；`scripts/build.sh` 成功构建 dust-hpm6750
  固件，ROM 86612 B、RAM 107672 B。§7 记录的 87484/110668 B 来自旧目录构建，
  不作为本工作区的度量。
- 根目录重构（含移除 message 生成器、代码格式化）与 UART 统一异步 DMA 接收
  已作为两个独立提交落库，提交信息分别为 `refactor!: rebuild the firmware as
  a root Zephyr application` 和 `feat(uart): unify all RX ports on asynchronous
  DMA double buffering`。本文 §2 的 HEAD 和 §7 的“尚未重新构建”是撰写时状态。
- UART10 的 TRGM1/GPTMR2 空闲检测已从应用 `src/board/` 下沉到 hpm_support
  UART 驱动：应用只调用 `uart_hpm_rx_line_idle_prepare()` /
  `uart_hpm_rx_line_idle_start()`，板级 DTS 提供 `uart10_idle_timer`，配置项为
  `CONFIG_UART_HPMICRO_RX_LINE_IDLE`（dust-hpm6750 默认 y）。hpm_support
  `main` 和 `west.yml` 均固定到 `d85eefb`。
- 本次仍未刷板、未使能电机、未做硬件连机/延迟/停机验收；§8 的验收顺序不变。
