# dust-adapter 固件架构 v2（重构提案）

> 状态：已确认并实施 · 2026-11-02 · 基于 `dust-adapter` 分支 `ethercat` 当前代码理解
>
> 本文不是对现有目录的"小修小补"，而是**保留外部约束、替换内部结构**的重新设计：
> 先给出对现有外设/服务关系与逻辑的完整理解，再给出新架构、接口、目录、构建、
> 测试与迁移路径。

> 后续演进（2026-11-04）：按维护者决策，已删除宿主机测试与端口抽象层。
> 当前代码以仓库为准：`common/ → domain/ → drivers/ → adapters/ → app`。
> 不再保留 `ports/`、`adapters/zephyr/`、`board/` 与 `tests/unit`；
> 单板资源/DTS 别名并入 `composition.cpp`，启动身份在 `src/identity.cpp`，
> 板级输出在 `drivers/pwm/outputs.cpp`。
> domain 已按 `protocol`（会话/线协议/CAN+UART 子协议）、`pdo`、`can`、`uart`、
> `protection` 重组；`io`/`feedback`/`layout_service` 移入 SSC 适配器。

## 已确认决策（2026-11-02）

| 决策 | 结论 | 对设计的影响 |
|---|---|---|
| ABI | **5.0 冻结** | 线协议/布局 CRC/能力位/诊断字段完全不变；`domain/wire` 只做内部整理，不改字节 |
| 迁移方式 | **一步到位替换** | 不做新旧树并存；完成后删除旧 `src/io`、旧 `gateway.cpp`、旧 `runtime.c` 等路径 |
| 加热器故障 | **本地维护** | 加热器超温/传感器失效只关加热 + 诊断，不上报为 EtherCAT 会话故障，不触发 safe-stop |
| 硬件看门狗 | **暂不引入** | `Lifecycle` 不接 IWDG；周期/存活由软件租约与 SM 看门狗保证 |
| 产品形态 | **单板单产品** | `board/` 只保留 `dust_hpm5e31` 一套资源表，不做多板抽象层，避免过度设计 |

---

## 0. 前提与约束（不可变项）

设计从以下约束出发，这些是重做架构时必须尊重的"物理与商业边界"：

| # | 约束 | 依据 |
|---|---|---|
| C1 | 硬件固定：HPM5E31 片上 ESC + RTL8201F/TJA1101B + 4×MCAN + 6×UART + SPI3 IMU + PWM0 两路 | `boards/dust-hpm5e31.overlay`、`docs/hpm5e31-board.md` |
| C2 | EtherCAT 从站必须继续使用已授权的 Beckhoff SSC 5.13，FreeRun 模式，单 SSC 执行上下文 | `src/ecat/generated/`、`docs/ethercat.md` |
| C3 | 主机 `dust-control` 与固件共享 PDO ABI 5.0 与布局 CRC 规则；外部线协议不能在内部重构中被意外改变 | `docs/ethercat-pdo-protocol.md` |
| C4 | 周期最快 125 µs，端到端以 1 kHz 为主；保护/租约语义是安全相关行为 | `config/pdo_schema.yaml`、`docs/ethercat-protocol-core.md` |
| C5 | 不允许运行期堆分配；异常/RTTI 关闭；静态资源可预算 | 现有代码与测试约束 |
| C6 | 编译期配置由 YAML/JSON 生成（CAN 白名单、PDO schema、SII/ESI），生成物必须可复现、可校验 | `tools/`、`docs/architecture.md` |
| C7 | Zephyr 与 hpm_support 版本由 `west.yml` 固定；不重写 Zephyr 驱动 | `west.yml` |

> 若允许改变 C2/C3（例如迁 DC 同步、改 ABI 6.0、换从站栈），本文对应位置标注了
> 扩展点，但默认不假设。

---

## 1. 现状理解：外设清单

| 外设 | 实例 / DTS 别名 | 关键参数 | 当前驱动落点 | 关键约束 |
|---|---|---|---|---|
| 片上 ESC | `esc0`，SM0 0x1000/128B、SM1 0x1080/128B、SM2 0x1100/≤1024B、SM3 0x1d00/≤1536B | PDI/RESET IRQ；EEPROM 模拟；SII 2 KB RAM 镜像；PA 地址 0x1100 起三缓冲 | `src/ecat/esc/*` | FreeRun 不启用 SM/DC 同步；SYNC0/1 启动后关闭 |
| 外部 PHY | Port0 RTL8201F（MDIO 地址 1）、Port1 TJA1101B（地址 0） | MDIO/MDC 共享；TJA 复位 PF28、中断 PF26 | `hpm_support` 的 `hpm5e31_phy.c` + `src/ecat/esc/esc_boot.c` | 专有初始化/链路检测需实板验收 |
| CAN | `dust_can_0..3` → `mcan0..3` | 1 Mbit/s（YAML 可配）；RX 过滤器来自 YAML；TX 批次 + deadline + abort | `src/drivers/can_driver.*`、`src/io/can_adapter.cpp` | 生产只用 CAN0/CAN3；单总线 ≤16 帧/批；保护帧必须可发 |
| UART | `dust_uart_1..6` → `uart1..6` | UART2 921600（HI91）；UART5 100000/8E2、仅 RX（SBUS）；异步 RX idle 200 µs + DMA 双 128B；TX 块 ≤512B | `src/drivers/uart_driver.*`、`src/drivers/uart_receive.*`、`src/io/uart_adapter.cpp` | 逻辑口 5 无 TX；每口独立窗口 |
| 板载 IMU | SPI3 + XDMA6/7，CS=PC21，DRDY=PC18 | 25 B 突发（0x0C 起）、身份 0x6A、温度 0x22/0x23（`raw/512+23`） | `src/drivers/imu_sensor.cpp` | HXY 寄存器序列未实板确认 |
| 板级输出 | 蜂鸣器 PWM0 ch1（PF01）；加热 PWM0 ch4（门极 PD04） | 加热 PID 默认 10 ms / 45 ℃ | `src/drivers/buzzer.c`、`board_pwm.c`、`imu_heater.cpp` | PWM 寄存器更新需互斥串行化 |
| 非易失存储 | `ethercat_eeprom_partition`（预留 Flash，NVS 2×4 KB） | boot 计数（magic + UID tag + counter） | `src/ecat/boot_identity.cpp` | 先提交并读回，才暴露 boot_id；不写 SII/Flash EEPROM |
| 芯片身份 | OTP UUID（`OTP_SOC_UUID_IDX`） | FNV-1a 派生 identity | 同上 | 全 0/全 F 拒绝启动 |
| 调试 | RTT 控制台（16 KB up） | 日志/诊断 | `prj.conf` | 实时路径不得同步打印 |

**外设到服务的抽象要点**：MCU 只做"搬运 + 本地保护"，不解析电机协议、不做姿态解算；
所有外设访问都应经过"端口"，业务层不出现 `device*`、DT 别名、寄存器偏移。

---

## 2. 现状理解：服务清单

| 服务 | 职责 | 关键状态 | 当前落点 | 执行上下文 |
|---|---|---|---|---|
| EtherCAT 栈生命周期 | `MainInit/MainLoop`、ESC/PHY/SII 启动、reset/rearm、IRQ 门控 | `stack_ready`、`reset_requested` | `src/ecat/runtime.c`、`esc_boot.c` | 主线程 + RESET ISR |
| 对象字典 / CoE | 0x2100 配置、0x1600/0x1a00 映射、0x1c12/0x1c13 分配、SDO Read/Write、对象描述 | 对象表、mapping staging、frozen | `src/ecat/gateway.cpp`、`src/protocol/pdo_dictionary.*` | 主线程（SSC 回调内） |
| 配置事务 / 布局 | 22 项参数校验、候选布局生成、CRC、与真实映射比对、三缓冲预算 | `ConfigurationTransaction`、`Layout/ImageLayout` | `src/protocol/layout.cpp` | 主线程 |
| 会话协议核心 | Discover/Arm/Stop、boot/session/exchange、租约、UART 窗口、PDO 编解码、状态回复 | `Core`（≈25.9 KB 静态实例） | `src/protocol/core.cpp` | 主线程独占 |
| CAN 服务 | 白名单校验、批次入队/发送、deadline/abort、保护帧、按 ID 最新反馈槽（20 ms 过期）、统计 | 每总线 32 事件环 + 32 发送队列 + 32 反馈槽 | `can_adapter.cpp` | ISR 入队，主线程排空/发送 |
| UART 服务 | 4 块发送窗口、序号/ACK、重传轮转、缺口标记、20 ms 超时、RX 队列 | 每口窗口 + 4 块 RX 队列 | `uart_adapter.cpp` | ISR/DMA 入队，主线程推进 |
| 反馈聚合 | CAN 快照 + UART 块 + IMU 最新突发 → Publish | `latest_imu_*`、`batch` | `io_backend.cpp` | 主线程 |
| 保护 / 本地停机 | 故障锁存、capability bit31、白名单/保护帧/队列清理 | `Core.state/fault` + 各层原子 `fault` | `core.cpp` + 各适配器 + `io_backend.cpp` | 主线程（锁存） |
| 启动身份 | OTP UID + NVS 递增计数 → boot_id | `boot`、NVS mounted | `boot_identity.cpp` | 主线程（初始化） |
| 板级传感器/热控 | IMU 采样、温度喂给 PID、加热/蜂鸣输出 | `latest_imu_*`、加热 fault | `imu_sensor.cpp`、`imu_heater.cpp`、`io_backend.cpp` | 主线程采样 + 系统工作队列 PID |
| 状态 / 诊断 | 状态目标/通道/分页路由、计数器、RTT 诊断 | 每通道计数、`diag_events` | `io_backend.cpp`、适配器 `*Status` | 主线程 |
| 构建期配置 | YAML/JSON → CAN profile、PDO schema、SII/ESI | 生成物 | `tools/`、`CMakeLists.txt` | 构建期 |

---

## 3. 现状理解：关系、时序与不变量

### 3.1 启动

```
复位
 └─ esc_init(): SII 装载 → EEPROM 模拟就绪 → PHY 复位/配置 → 链路更新 → 注册 PDI/RESET 钩子
 └─ buzzer_init()
 └─ ecat_runtime_start(): SSC MainInit() → gateway_init()
      └─ InitializeIo(boot): CAN 队列/初始化 → UART 初始化 → IMU 初始化 → 读 boot 计数
      └─ Core 构造(protection = IoProtection()) → 注册 0x2100 等对象 → ApplyLimits()
 └─ imu_heater_init()
 └─ ecat_runtime_enable(): stack_ready=1 → ESC start（关 SYNC0/1 IRQ）
 └─ 主循环
```

### 3.2 主站配置 → 会话建立（PREOP → SAFEOP → OP）

```
主站 SDO：0x2100:1..22 参数（顺序任意）
        → 0x1600/0x1a00 映射条目（先写 sub0=0 禁用，写条目，再写 sub0=数量）
        → 0x1c12/0x1c13 分配对象
        → APPL_GenerateMapping()
             PdoDictionary.Snapshot() → ConfigurationTransaction.Commit()
             （22 项齐全 + CRC + 与真实映射逐项一致 + 三缓冲预算）
             → Describe 对象 → 返回 input/output 大小
        → 主站请求 SAFEOP：APPL_StartInputHandler()
             mapping_ready + CheckSm()（地址/长度/方向/三缓冲/看门狗）+ 同步类型
             → Core.Configure() → ConfigureIo() → dictionary.Freeze()
        → 主站请求 OP：APPL_StartOutputHandler()（state==Ready）
        → APPL_OutputMapping() 捕获新 Rx 影像 → APPL_Application():
             Core.Process() → Actions → io_backend.DispatchIo() → CAN/UART 提交
```

### 3.3 周期上下行

```
下行：ESC RxPDO → APPL_OutputMapping → rx_image
     → Core.Process(now, operational)：校验会话/boot/exchange/来源/租约/白名单
     → Actions{can[2], uart[6], status}
     → DispatchIo：保护检查 → CAN 批次（deadline=min(批次, now+20 ms)）→ MCAN
                    → UART 块入队 → 异步发送
上行：CAN ISR → k_msgq；UART ISR/DMA → k_msgq
     → 主线程 Drain：CAN 按 ID 更新 32 槽（20 ms 过期）→ batch++
                      UART 取块 → Core.QueueUart()（4 块窗口，序号/缺口）
     → CaptureIo：反馈快照 + IMU 最新突发
     → Core.Publish()：公共头/能力/状态/fault/CAN/UART/IMU/诊断 → publish_sequence++
     → APPL_InputMapping → tx_image → ESC SM3
保护：租约超时/运输失效/来源期限/UART 20 ms/白名单失败/映射不一致
     → 立即锁存 Protected → 同步 stop 回调（清队列、CAN abort/保护帧、UART abort）
     → 只允许集成层在安全上下文重建（rearm），普通 PDO 无法清除保护
rearm：APPL_StopInputHandler（会话结束）→ rearm_pending
     → 主循环：屏蔽 ESC IRQ → stack_ready=0 → 新 boot_id/清映射/清影像 → 重新 MainInit
```

### 3.4 必须保持的行为不变量（新架构逐条继承）

1. **单执行上下文拥有 Core/SSC/队列/窗口/ESC 寄存器**；ISR 只入队，不调业务。
2. Arm 之前必须验证本地保护已安装（白名单 + stop 回调），否则不声明 capability bit31。
3. 每个周期（即使无新 RxPDO）都要 `Tick(now, transport_valid)`；`transport_valid`
   取 AL 状态 + 有效过程数据 + SM 看门狗，不能只看网线。
4. UART 先复制完整块并预留空间，再 `CommitUart()` 推进 ACK；不允许先确认后入队。
5. 同一 exchange 重复影像不派发、不续期；内容变化/乱序进入保护。
6. 保护锁存不可逆；停机顺序确定，重复调用幂等。
7. boot_id 先持久化并读回，才对外暴露；会话/重保重建必须换新 boot_id。
8. 125 µs 周期下实时路径不阻塞、不打印、不分配堆。

### 3.5 现状架构的结构性问题（新设计的直接输入）

| # | 问题 | 表现 |
|---|---|---|
| S1 | io ↔ ecat 链接期循环依赖 | `io_backend.cpp` 前置声明并调用 `ecat/boot_identity.cpp` 的 `AllocateBootIdentity`；`gateway_init` 又调用 `InitializeIo` |
| S2 | 单一"上帝适配器" | `gateway.cpp` 630 行同时做 OD、SDO、PDO 映射、SM/寄存器读取、发布策略、rearm |
| S3 | 保护策略分散 | 32 处 `Protect(...)` 分布在 protocol/io/ecat，无唯一停机编排 |
| S4 | 板级特性渗入通用层 | `io_backend.cpp` 用 `#if CONFIG_ONBOARD_IMU_RAW` 直接 own IMU 并走 `extern "C"` 喂加热器 |
| S5 | 对象索引三份副本 | `pdo_schema.hpp`、`layout.cpp NoteRegion`、`gateway.cpp` 对象列表/分界各自硬编码 |
| S6 | 跨 TU 接口靠临时声明 | `AllocateBootIdentity`、`imu_heater_*` 无统一头文件；死接口 `CanReceivePending` |
| S7 | 边界约束靠正则脚本，实测可绕过 | `check_driver_boundary.py` 中 `FORBIDDEN_PREFIXES` 未使用；相对 include 不检查 |
| S8 | 板级 `#if` 与 CMake `if` 混用 | 同一类特性两种开关方式，通用代码里出现板级条件编译 |
| S9 | 状态来源多重 | `Core.state`、`capturing/ready`、适配器 `ready/fault` 手工同步 |

---

## 4. 设计需求（由上述约束与问题推导）

| ID | 需求 | 验收方式 |
|---|---|---|
| DR1 | 依赖必须是无环 DAG：`domain` 只依赖端口接口，不依赖 Zephyr/drivers/adapters | CMake target 链接图 + 编译测试 |
| DR2 | 保护/停机有且只有一个所有者，所有故障先汇聚再锁存 | 代码检索 + 单元测试 |
| DR3 | SSC 只出现在一个适配器内；其他模块不 include SSC 头 | 边界检查（按 target，不靠正则） |
| DR4 | 板级差异通过"组合"表达，通用代码零板级 `#if` | grep 检查 + 多板构建 |
| DR5 | 生成物是对象索引/布局常量的唯一来源；生成物可复现、可校验 | 生成-比对测试 |
| DR6 | domain 可在宿主机无硬件、无 Zephyr 运行，用 fake 端口覆盖故障路径 | 原生测试 |
| DR7 | 生命周期状态机唯一，所有 AL/IRQ/队列状态由它派生 | 状态转换测试 |
| DR8 | 实时路径可测量：每阶段耗时、最坏延迟、栈水位可观测 | 运行时统计 + 台架报告 |
| DR9 | 保留 ABI 5.0 与现有验证逻辑，重构不改变外部行为 | 黄金向量 + 主从 crosscheck |
| DR10 | 板级能力（通道数、RAM 预算、SM 分区）集中在 board 资源表 | board 单一文件审查 |

---

## 5. 新架构总览

### 5.1 一句话

**单归属实时循环（Single-Owner Loop）+ 端口/适配器（Ports & Adapters）+ 领域服务（Domain Services）**
：一个实时线程拥有全部业务状态；领域服务只依赖纯接口；SSC、Zephyr 驱动、板级资源都是可替换的外围适配器；生成物是常量的唯一来源。

### 5.2 分层图

```
┌──────────────────────────────────────────────────────────────────────┐
│ app/  (组合根 + 调度)                                                 │
│   main.cpp  scheduler.cpp  composition.cpp                           │
└───────────────┬──────────────────────────────────────────────────────┘
                │ 只允许 app 依赖所有层
┌───────────────▼──────────────────────────────────────────────────────┐
│ adapters/  （SSC / ESC 适配）        drivers/（硬件原语+端口实现）    │
│   ethercat_ssc/   zephyr/   storage/                                 │
└───────┬───────────────────────────────┬──────────────────────────────┘
        │ 实现 ports                     │ 使用 drivers
┌───────▼───────────────────────────────▼──────────────────────────────┐
│ ports/  （纯接口，零依赖）        drivers/  （窄硬件原语，只依赖 platform）│
│  ICanBus IUartPort IImuSource    mcan/ uart/ spi_imu/ pwm/ nvs/ esc/ │
│  IClock IStorage IOutputs ...                                        │
└───────▲───────────────────────────────▲──────────────────────────────┘
        │ 只依赖 ports                  │
┌───────┴──────────────────────────────────────────────────────────────┐
│ domain/  （纯 C++17，可宿主机测试，无 Zephyr / SSC / DT）              │
│  lifecycle  session  wire  layout  can_io  uart_io  feedback          │
│  protection  identity  observability  outputs                        │
└──────────────────────────────────────────────────────────────────────┘
        ▲
        │ board/ 提供纯数据资源表（设备引用、通道、预算、拓扑、能力）
```

**依赖规则（编译期强制，不靠脚本）**：

| target | 允许依赖 |
|---|---|
| `dust_ports` | 无（仅标准库） |
| `dust_domain` | `dust_ports` |
| `dust_platform` | Zephyr |
| `dust_drivers` | `dust_platform` + HPM SDK |
| `dust_adapters_zephyr` | `dust_ports`、`dust_drivers`、`dust_platform` |
| `dust_adapters_ssc` | `dust_ports`、`dust_domain`、SSC 源码（仅此 target） |
| `dust_app` | 以上全部 |

违反依赖 = 链接失败（比"include 扫描"硬得多）。

### 5.3 为什么不是别的方案

| 备选 | 否决理由 |
|---|---|
| 每个服务一个 RTOS 线程 + 锁 | 125 µs 下锁竞争/优先级反转风险大；SSC 本身要求单上下文；调试成本高 |
| 全局事件总线 / 发布订阅 | 隐藏调用关系、生命周期难追踪；单线程下没有收益 |
| 无接口、全靠全局函数（现状） | 无法在宿主机替换硬件；故障路径不可测；板级 `#if` 扩散 |
| 模板化静态注入（无虚函数） | 可行且有性能优势，但 fake 成本高；默认用虚接口，若实测开销超预算再局部改模板 |

---

## 6. 目录结构与构建目标

```
src/
├── app/
│   ├── main.cpp                 # 唯一 main；板级初始化顺序
│   ├── composition.cpp/.hpp     # 组装：board + adapters + services，注入端口
│   └── scheduler.cpp/.hpp       # 实时循环策略（Active/Idle/Fatal，慢任务到期）
├── domain/                      # 纯逻辑，可 host 编译
│   ├── lifecycle/               # 唯一状态机；会话建立/停止/重保
│   ├── session/                 # Discover/Arm/Stop、租约、状态回复、发布策略
│   ├── wire/                    # ABI 5.0 编解码、字节序、能力位、诊断字段
│   ├── layout/                  # 配置事务、布局构建、CRC、SM 预算校验
│   ├── can_io/                  # 白名单、批次、deadline、反馈槽、统计
│   ├── uart_io/                 # 窗口、ACK、重传、缺口、超时
│   ├── feedback/                # 多源反馈聚合（CAN/UART/IMU）
│   ├── protection/              # 故障汇聚、分级、停机编排、幂等
│   ├── identity/                # boot 计数策略（持久化由端口实现）
│   ├── observability/           # 状态分页注册、计数器、事件日志、耗时统计
│   └── outputs/                 # 加热 PID/蜂鸣策略（慢任务）
├── ports/                       # 纯接口头（无 .cpp）
│   ├── clock.hpp  can_bus.hpp  uart_port.hpp  imu_source.hpp
│   ├── outputs.hpp  storage.hpp  device_identity.hpp
│   ├── ethercat_stack.hpp  status_provider.hpp  logger.hpp
│   └── fault_sink.hpp           # domain 内部接口也可放这里统一
├── adapters/
│   ├── ethercat_ssc/            # 唯一包含 SSC 的地方（stack/callbacks/dictionary/mapping）
│   └── esc/                     # ESC 寄存器/SII/PHY/启动
├── drivers/                     # 窄硬件原语 + CAN/UART/IMU 端口实现
│   ├── can/ uart/ spi_imu/ pwm/
├── board/
│   ├── heater_config.hpp / heater_pid.hpp
│   └── dust_hpm5e31/
│       ├── board_resources.cpp  # 通道表、波特率、过滤器、保护帧、Limits、能力
│       ├── identity.cpp         # 启动身份（直接 OTP/NVS）
│       └── outputs.cpp          # 板级输出（直接 PWM/蜂鸣器 + 加热 PID）
└── platform/                    # 时间、临界区、IRQ、assert、日志原语
```

`config/`、`tools/`、`tests/` 保留，但构建接线收敛到 **一个** CMake 配置模块
（`cmake/adapter_config.cmake`），固件与测试共用，不再两边各写一份生成命令。

---

## 7. 端口定义（关键接口草案）

端口只描述"服务需要什么"，不描述"硬件怎么实现"；全部纯 C++17、无堆、无异常。

```cpp
// ports/can_bus.hpp
struct CanFrame {
    uint8_t  bus;
    uint32_t id;
    uint8_t  length;
    std::array<uint8_t, 8> data;
    uint32_t capture_us;   // 由适配器在 ISR 打时间戳
};

class ICanBus {
public:
    virtual ~ICanBus() = default;
    virtual Error Init(const BoardCanConfig& cfg) = 0;
    // 同步提交一批；失败表示未完全接纳，服务需按保护策略处理
    virtual Error SubmitBatch(uint8_t bus, const CanFrame* frames,
                              size_t count, uint64_t deadline_us) = 0;
    virtual Error AbortAll(uint8_t bus) = 0;
    virtual bool  DrainRx(CanFrame& out) = 0;         // SPSC，非阻塞
    virtual CanBusHealth Health(uint8_t bus) const = 0;
};

// ports/uart_port.hpp
class IUartPort {
public:
    virtual Error Init(const BoardUartConfig& cfg) = 0;
    virtual Error SubmitBlock(uint8_t port, const uint8_t* data, size_t len) = 0;
    virtual Error Abort(uint8_t port) = 0;
    virtual bool  DrainRx(uint8_t& port, RxBlock& out) = 0;  // 带 capture_us/gap
    virtual bool  TxIdle(uint8_t port) const = 0;
};

// ports/ethercat_stack.hpp —— SSC 适配器实现；domain 只看到这些语义
class IEthercatStack {
public:
    virtual Error Start(const StackConfig&) = 0;  // MainInit + OD 注册
    virtual void  Stop() = 0;                     // 关 IRQ，保 SII RAM
    virtual void  Step() = 0;                     // MainLoop；回调内驱动 domain
    virtual void  RequestRearm() = 0;             // 安全上下文重建
    virtual bool  TakeRxImage(MutableBytes& out) = 0;   // APPL_OutputMapping 后置位
    virtual void  PutTxImage(Bytes in) = 0;             // 由 APPL_InputMapping 取走
    virtual bool  TransportHealthy() const = 0;   // AL==OP + 有效过程数据 + SM 看门狗
    virtual AlState Al() const = 0;
};
```

其余端口：`IClock`（µs/cycle）、`IImuSource`（TryTakeSample）、`IOutputs`（SetHeaterDuty/SetBuzzer）、
`IStorage`（记录读写，NVS 语义由适配器实现）、`IDeviceIdentity`（读 UID）、
`IStatusProvider`（target/channel/page → 3×u32）、`ILogger`（ISR 安全环形日志）。

**Domain 内部接口**：`IFaultSink`（`Report(FaultCode, source)`），由 `ProtectionService`
实现并注入各服务——这样服务永远不直接"锁存保护"，只上报；保护决策集中。

---

## 8. 领域服务设计

### 8.1 LifecycleService（唯一状态机）

状态：`Boot → SelfTest → Configurable(PreOp) → Ready(SafeOp) → Active(Op) → Protected → Rearm → Configurable`，另有 `Fatal`。

- 持有"会话世代"（boot_id / session_id / 配置是否冻结）。
- 输入：SSC 适配器的 `ResetRequested`、`SessionStopped`、AL 状态、保护锁存事件、rearm 请求。
- 输出：调用 `IEthercatStack::Start/Stop/RequestRearm`；调用 `IdentityService` 取新 boot_id；
  调用 `ProtectionService::EnterSession/ClearForRearm`；调用 IO 服务的 `EnterActive/Stop`。
- **所有 `stack_ready/capturing/rearm_pending` 类标志消失**，改由本状态机派生。

### 8.2 SessionService（协议核心，从现有 `Core` 迁移）

- 保留现有验证语义（session/boot/exchange/digest、来源期限、Stop 判据、UART 窗口、PDO 编解码）。
- 拆成两个内聚单元：
  - `SessionState`：状态与授权/期限（`Process/Tick/Protect` 逻辑）；
  - `WireCodec`：`Publish` 的字段编码、`DecodeHeader`、能力位/诊断（纯函数，输入/输出 Bytes）。
- 对外：`Configure(layout)`、`Process(rx, now) -> CommandPlan`、`Tick`、`Publish(tx, feedback)`、
  `CompleteStatus`、`QueueUart/CommitUart`、`State()`。
- 测试：沿用现有合约测试并补黄金向量（与 host 共享）。

### 8.3 LayoutService（配置事务 + 布局）

- `ConfigurationTransaction`（0x2100 staging）与 `BuildLayout` 保留，但对象索引常量**只从生成头取**。
- 输入：`Limits`（来自 board）、`PdoMappingSnapshot`（来自 SSC 适配器）。
- 输出：`Layout`（各区域偏移）、CRC 签名、SM 预算校验。
- 生成器同时输出 `kObjectBase*` 常量表，`NoteRegion`/OD 注册/`Read` 分界全部引用它，
  消除 S5 的三份副本。

### 8.4 CanIoService

- 职责：把 `CommandPlan.can` 变成硬件批次；维护每总线"每 ID 最新反馈槽"与统计。
- 内部状态：每总线 `TxQueue<32>`、`LatestSlots<32>`、`batch_sequence/drops`。
- 依赖：`ICanBus`（提交/abort/drain）、`IFaultSink`、`IClock`。
- 保护语义：批次 deadline 取 `min(协议 deadline, now + command_timeout)`；提交失败/abort 失败
  → 上报 fault（不再自己调 `Protect`）。保护帧由 `ProtectionService` 通过
  `CanIoService::EmitProtectionFrames()` 触发，保证顺序。
- 测试：fake `ICanBus` 模拟 EAGAIN/abort 失败/总线 Off，验证队列与保护帧顺序。

### 8.5 UartIoService

- 职责：下行 4 块窗口、上行队列、ACK 提交、缺口/超时。
- 依赖：`IUartPort`、`IFaultSink`、`IClock`。
- 规则：只有 `SubmitBlock` 成功并预留空间后才 `SessionService::CommitUart`；
  RX 块带 `capture_us` 与 `gap`；20 ms 未完成 → fault。
- 测试：fake 端口注入 DMA 完成顺序、重复 ACK、缺口、回绕。

### 8.6 FeedbackAggregator

- 每个发布周期采样 `CanIoService::Snapshot()`、`UartIoService::Snapshot()`、`IImuSource::TryTakeSample()`、`OutputsService::Temperature()`。
- 输出 `Feedback`（与现有 `Core::Feedback` 同构）。
- 不拥有 IMU 采样策略；只做聚合与有效性判断。**S4 被消除**。

### 8.7 ProtectionService（唯一停机编排）

- 故障分级：
  - `SessionFatal`（运输/租约/序列/映射/白名单/UART 超时）→ 锁存 Protected，执行全量 safe-stop；
  - `LocalActuator`（加热器超温/传感器失效）→ 关加热 + 诊断，不必然影响 EtherCAT 会话；
  - `Recoverable`（统计类）→ 计数/日志。
- 停机顺序（幂等、固定）：
  1. `SessionService::Latch(reason)`（拒绝新命令）
  2. `UartIoService::AbortAll()`
  3. `CanIoService::AbortAll()` + `EmitProtectionFrames()`
  4. `OutputsService::SafeState()`（加热=0、蜂鸣按策略）
  5. 记录 `fault`/计数/诊断
- capability bit31 只在"白名单校验通过 + 全部 safe-stop 端口已安装"时置位。
- 测试：故障注入 → 验证顺序、幂等、reason 映射、诊断字段。

### 8.8 IdentityService

- 读取 `IDeviceIdentity`（OTP UID）→ FNV-1a → identity；`IStorage` 读/写/读回计数器；
  `NextBootIdentity` 策略保留；失败返回明确错误，不提供可 Arm 的会话。
- **移到 domain 后，io 不再反向依赖 ecat（S1 消除）**。

### 8.9 ObservabilityService

- `IStatusProvider` 注册表：`(target, channel, page)` → 3×u32；
  CAN 统计、UART 统计、系统统计（uptime、循环耗时、栈水位、保护计数）各自注册。
- 状态回复：`SessionService` 只发 `StatusRequest{target,channel,page,seq}`，
  由本服务路由；`io_backend` 的大 if/else 消失。
- 事件日志：固定 64 条环形（时间戳+码+参数），RTT 按需 dump；不阻塞实时路径。

### 8.10 OutputsService（慢任务）

- 10 ms 周期：读取最新 IMU 温度 → PID → PWM 占空比；>200 ms 无样本 → 关加热 + fault。
- 蜂鸣器：由诊断/启动事件触发，异步有界。
- 运行在系统工作队列（与实时线程通过 mutex 保护的"最新温度快照"交互，保持现有边界），
  策略仍在 domain 内，Zephyr 细节在 adapter。

---

## 9. SSC 适配器：把 630 行拆成翻译层

| 文件 | 职责 | 行数量级 |
|---|---|---|
| `stack.cpp` | `MainInit/MainLoop`、reset/rearm、IRQ 门控、`stack_ready` 等价物 | ~120 |
| `callbacks.cpp` | 10 个 `APPL_*`：只做影像复制/事件上报/读取结果，不含业务判断 | ~150 |
| `object_dictionary.cpp` | TOBJECT 表构建（索引来自生成头）、SDO Read/Write 转发到 `LayoutService` | ~180 |
| `mapping.cpp` | `GenerateMapping/Describe/CheckSm/ApplyLimits` | ~120 |
| `sii.cpp` | SII RAM、EEPROM 命令循环、reload 响应 | 平移现有 |
| `esc_*.c` | ESC 寄存器/PHY/启动（来自现有 `ecat/esc`） | 平移 |

关键变化：
- `APPL_StartInputHandler` 只返回 `LifecycleService::OnSafeOpRequested()` 的结果；
- `APPL_Application` 只调用 `SessionService::OnOutputsReady()`；
- `Read/Write` 的偏移/分界使用生成常量；
- 不再有 `gateway.cpp` 级别的"发布策略三份实现"。

---

## 10. 执行模型

| 上下文 | 职责 | 规则 |
|---|---|---|
| 实时主线程（prio 0） | SSC `Step()`、domain 服务全部调用、硬件提交 | 唯一拥有业务状态；不阻塞、不打印、不分配 |
| ESC PDI ISR | EEPROM 模拟 + 通知 PDI；置位事件标志 | 有界；不解析业务 |
| ESC RESET ISR | 置位 `reset_requested` | 只置位 |
| CAN RX ISR | 时间戳 + 写入每总线 SPSC 环（丢弃最旧） | 不发送、不调 domain |
| UART ISR/DMA | 组块入队/缺口/完成标志 | 不覆盖在途缓冲，不推进 ACK |
| IMU DRDY GPIO ISR | 记录 cycle 时间戳入 1 深度消息队列 | 只记录 |
| SPI DMA 完成 | 置位完成/状态原子量 | 不拷贝业务 |
| 系统工作队列 | 加热 PID（10 ms）、蜂鸣、日志 drain | 只拿温度快照，不碰 ESC/过程数据 |

**队列/同步规则**：ISR→线程一律 SPSC 有界环（`Ring<T,N>`，acquire/release，满则丢弃最旧并计数）；
domain 内单线程，无锁；跨线程仅温度快照 mutex 与日志环形。每个环有明确的"单写者/单读者"注释与测试。

**实时预算（目标，需台架实测）**：125 µs 周期内主循环业务 ≤ 40 µs（P99）；
`Process+Publish` ≤ 15 µs（沿用现有 `-Os` 栈报告作为基线）；ISR 全路径 ≤ 3 µs；
日志/诊断只在 Idle 分支或低优先级工作队列执行。

**可观测性**：`CycleStats` 记录每阶段最近/最大耗时、超期计数、栈水位；
通过状态分页暴露；台架脚本采样生成报告。

---

## 11. 生命周期状态机

```
                 ┌──────────┐
   reset ───────►│   Boot   │
                 └────┬─────┘
                      │ 自检：ESC/SII/PHY/IO/身份/NVS 读回
                 ┌────▼───────┐  身份/存储失败   ┌────────┐
                 │  SelfTest  ├─────────────────►│ Fatal  │
                 └────┬───────┘                  └────────┘
                      │ 全部就绪（capability 可声明）
                 ┌────▼──────────┐
                 │ Configurable  │◄──────────────────────────┐
                 │   (PREOP)     │   rearm 完成               │
                 └────┬──────────┘                           │
        APPL_StartInput│ 配置事务/映射/SM 通过                 │
                 ┌────▼──────┐                               │
                 │  Ready    │                               │
                 │ (SAFEOP)  │                               │
                 └────┬──────┘                               │
           APPL_StartOutput│                                  │
                 ┌────▼──────┐   任一 SessionFatal           │
                 │  Active   ├──────────────┐               │
                 │   (OP)    │              │               │
                 └────┬──────┘              │               │
                      │ Stop / 会话结束      │               │
                 ┌────▼──────┐         ┌────▼──────┐        │
                 │ Protected │────────►│  Rearm    │────────┘
                 │ (锁存)    │  主站下次启动│ (安全上下文)│
                 └───────────┘         └───────────┘
```

不变量：`Protected` 只能经 `Rearm` 离开；`Rearm` 必须换 boot_id；AL 状态是**输入**，
不是状态机所有者；IRQ 门控是状态机的输出（而非散落的 `stack_ready` 标志）。

---

## 12. 关键时序（新实现）

**周期循环（伪代码）**

```cpp
// app/scheduler.cpp —— 唯一实时循环
for (;;) {
    const StackEvents ev = stack.PollEvents();     // reset/pdi/session-stopped
    lifecycle.Handle(ev);                          // 状态机唯一入口

    if (lifecycle.CanExchange()) {
        session.Tick(clock.NowUs(), stack.TransportHealthy());
        Bytes rx;
        if (stack.TakeRxImage(rx)) {               // APPL_OutputMapping 之后
            CommandPlan plan = session.Process(rx, clock.NowUsUs());
            if (plan.valid) {
                protection.GuardCommandPlan(plan); // 校验+故障上报，不锁存业务
                can.Submit(plan.can);
                uart.Submit(plan.uart);
            }
        }
        if (session.PublishDue(clock.NowUsUs())) {
            Feedback fb = feedback.Sample();
            session.Publish(stack.TxImage(), fb);
            stack.PutTxImage();                    // 下一次 APPL_InputMapping 生效
        }
    }

    stack.Step();                                  // SSC MainLoop（会回调 APPL_*）
    observability.Tick();
    if (!lifecycle.Active()) {                     // Idle：慢任务
        outputs.TickIfDue();
        logger.Drain();
        link.PollIfDue();
    }
}
```

**保护时序**：`FaultSink::Report` → `ProtectionService` 置位并记录 → 下一行代码（同一线程）
执行 stop 编排 → `lifecycle` 进入 `Protected` → 发布携带 fault/state → 主站可见。
最坏延迟 = 一次循环（Active 下微秒级；Idle 下 ≤ 调度间隔）。

**rearm 时序**：会话结束事件 → `lifecycle` 请求 `stack.Stop()`（屏蔽 IRQ）→
`identity.NewBoot()`（提交+读回）→ `stack.Start()`（新 OD 注册）→
`layout.Reset()`/`session.Reset()`/`protection.ClearForRearm()` → `Configurable`。

---

## 13. 配置与代码生成流水线

```
config/pdo_schema.yaml ──► tools/generate_pdo_schema.py ──► build/gen/pdo_schema.hpp
                                                       └─► build/gen/pdo_objects.hpp（新增：对象基址/区域表）
config/can_devices.yaml ─► tools/generate_can_profile.py ─► build/gen/can_profile.hpp
config/ethercat_gateway.json ─► tools/generate_gateway_esi.py ─► build/gen/{gateway_config.h,eeprom.h,gateway.sii,ESI.xml}
```

规则：
1. **所有生成物只落在构建目录**（现有 `pdo_schema.hpp` 提交进仓库的做法改为构建期生成，
   或保留提交但加"重新生成并逐字节比较"的 CTest；二选一，默认构建期生成）。
2. 生成器同时输出对象基址，`layout/od/read 分界` 全部引用它（消除 S5）。
3. 每个生成器都有宿主测试：确定性、与 YAML 的语义等价、与主机共享的 golden 向量。
4. 构建与测试**共用** `cmake/adapter_config.cmake`，不再复制 custom command（消除构建重复）。

---

## 14. 构建结构与依赖强制

```cmake
add_library(dust_ports INTERFACE)
add_library(dust_domain STATIC ...)          # 只 link dust_ports
target_link_libraries(dust_domain PRIVATE dust_ports)
add_library(dust_platform STATIC ...)        # Zephyr
add_library(dust_drivers STATIC ...)         # link dust_platform
add_library(dust_adapters_zephyr STATIC ...) # link dust_ports dust_drivers dust_platform
add_library(dust_adapters_ssc STATIC ...)    # link dust_ports dust_domain + SSC objs
target_include_directories(dust_adapters_ssc PRIVATE ${SSC_DIR} ${GEN_DIR})
add_executable(app ...)                      # link 全部
```

CI 中的架构检查（脚本只是补充，主约束是链接图）：
- `dust_domain` 的编译单元不得 include `zephyr/`、`drivers/`、`adapters/`、SSC 头；
- `drivers/` 不得 include `domain/`、`ports/`、SSC 头；
- 生产者/消费者接口只出现在 `ports/`；
- 禁止 `#if defined(CONFIG_*)` 出现在 `domain/`（板级差异走组合）；
- `git grep` 禁止新增无头文件的 `extern` 声明。

---

## 15. 测试策略

| 层 | 手段 | 覆盖 |
|---|---|---|
| domain 单元 | fake 端口 + 虚拟时钟（纯 host） | 状态机转换、配置/布局、CAN/UART 窗口、保护顺序、identity 失败路径、回绕 |
| adapter 集成 | 现有 Zephyr fake 头 + 模拟 ISR 回调 | 驱动/端口实现、中断入队、DMA 完成顺序、abort |
| 合约 | CTest + Python | codegen 确定性/同步、ESI/SII、CAN topology、ABI golden 向量（与 dust-control 共享） |
| 等价性 | 参考实现 vs 优化实现字节比较 | `Publish` 优化开关（保留现有做法） |
| 架构 | target 链接 + 编译测试 | 分层、SSC 隔离、无板级 `#if` |
| 静态/资源 | `-fstack-usage`、`size` diff、no-heap 链接检查 | 栈/Flash/RAM 预算 |
| 台架 | probes + 脚本 | 周期抖动、最坏延迟、CAN/UART 吞吐、保护触发、长稳 |

CI 阶段：`arch → host unit → contract → firmware build → size budget → HIL smoke`。
任何阶段失败都阻断合并；实板结果不替代离线证据，离线通过也不宣称硬件验收。

---

## 16. 资源与实时性预算

- **静态分配**：所有服务实例、队列、影像缓冲静态/持久存储；`Core` 类实例约 26 KB（沿用基线），
  新设计把它拆成 `SessionState` + 固定缓冲，预算不增加。
- **栈**：主线程 8 KB、ISR 4 KB（沿用）；domain 不做大栈帧；生成物不进栈。
- **队列表**：CAN 每总线 32 事件/32 命令；UART 每口 4 块；反馈槽 32/总线；
  全部可编译期按 board 调整。
- **周期**：125 µs 下目标 P99 主循环 ≤ 40 µs；每阶段统计入状态分页。
- **启动**：SII/PHY/NVS 初始化保持现有顺序；boot 计数写失败拒绝启动。

---

## 17. 迁移映射与阶段计划

### 17.1 文件映射

| 现有 | 新位置 | 处理 |
|---|---|---|
| `src/protocol/protocol.hpp` `core.cpp` | `domain/session/*` + `domain/wire/*` | 拆分，逻辑保留 |
| `src/protocol/layout.cpp` `pdo_dictionary.*` `io_policy.hpp` | `domain/layout/*` | 逻辑保留，常量改生成 |
| `src/io/can_adapter.cpp` | `domain/can_io/*` + `drivers/can/can_bus.cpp` | 策略/机制分离 |
| `src/io/uart_adapter.cpp` | `domain/uart_io/*` + `drivers/uart/uart_port.cpp` | 同上 |
| `src/io/io_backend.cpp` | 删除；拆成 feedback/protection/observability/identity 注入 | S1/S4/S6 消除 |
| `src/ecat/gateway.cpp` | `adapters/ethercat_ssc/*` | 拆五份，OD 常量生成 |
| `src/ecat/runtime.c` `esc/*` | `domain/lifecycle/*` + `adapters/esc/*` | 状态机唯一化 |
| `src/ecat/boot_identity.cpp` | `board/dust_hpm5e31/identity.cpp`（直接 OTP/NVS） | S1 消除 |
| `src/drivers/*` | `drivers/*` + 板级输出/身份到 `board/` | 基本平移，边界更窄 |
| `src/ecat/generated/` | 不变 | 授权代码 |
| tests/unit | `tests/unit`（domain）+ `tests/integration`（adapters） | fake 端口替代 fake facade |
| tools/ + config/ | 不变，接线收敛 | 单一 CMake 模块 |

### 17.2 阶段（每阶段可独立验证，不是"边改边猜"）

| 阶段 | 内容 | 验证 |
|---|---|---|
| P0 | 冻结行为：补 ABI golden 向量、host 仿真骨架、codegen 同步检查 | 现有 27 项测试 + 新 golden |
| P1 | 建 `ports/`，把 protocol 层机械迁入 `domain/` | 现有合约测试改 include 后全绿 |
| P2 | 引入 `ProtectionService` + `LifecycleService`，删除散落 `Protect` 与多套标志 | 保护/状态机单测 |
| P3 | 拆 SSC 适配器，OD 使用生成对象基址；删除 `gateway.cpp` | SSC 合约测试 + 主从 crosscheck |
| P4 | CAN/UART/feedback 服务化，删除 `io_backend`/适配器全局函数 | fake 端口单测 + IO 集成测试 |
| P5 | board 资源表 + 组合根；domain 零板级 `#if`；CMake 分层强制 | 分层编译测试、多板构建 |
| P6 | 实时统计、台架长稳、HIL、清理旧代码 | 台架报告 + 验收清单 |

若担心一次性替换风险：新树可先与旧 `src/` 并存，用 `CONFIG_NEW_ARCH` 选构建目标，
P1–P5 期间保持旧固件可发布，P6 切换后删除旧路径（strangler 模式）。

---

## 18. 架构决策记录（ADR 摘要）

| ID | 决策 | 理由 |
|---|---|---|
| ADR-01 | 保留"单归属实时线程"，不引入多线程服务 | 125 µs 确定性、SSC 单上下文要求、无需锁 |
| ADR-02 | 用端口/适配器而非全局函数 | 硬件可替换、故障路径可宿主机测试、依赖可链接期强制 |
| ADR-03 | SSC 只存在于 `adapters/ethercat_ssc` | 授权/第三方代码隔离，业务层零 SSC 依赖 |
| ADR-04 | 生成物是对象索引/布局常量的唯一来源 | 消除三份副本与漂移 |
| ADR-05 | 保护/停机唯一所有者 | 可审计、可测试、顺序确定、幂等 |
| ADR-06 | 板级差异用组合，不用通用代码 `#if` | 避免 S4/S8 类扩散；多板成本最低 |
| ADR-07 | 保留 ABI 5.0 与现有协议逻辑 | 外部契约稳定，重构可独立验证 |
| ADR-08 | 默认虚接口注入；若实测开销超标再局部模板化 | 可测性优先，性能有预算和退路 |

---

## 19. 风险、开放问题与验收标准

**风险**
- 拆 SSC 适配器会触碰最敏感的中断/时序路径 → 用现有 SSC 合约测试 + 主从 crosscheck 兜底。
- 生成对象基址需要 generator 与 SSC OD 表严格一致 → 加生成物一致性测试与运行时断言（DEBUG）。
- 单线程循环内服务增多可能推高 P99 → 先做阶段耗时基线，再决定是否把 observability/慢任务移到工作队列。

**开放问题（需你拍板）**
1. ABI 5.0 是否冻结？若要改（例如状态改为事件推送、增加 IMU 标定字段），应单独起 ABI 6.0 设计。
2. 是否接受"新树并存、strangler 切换"的过渡方式，还是一次性替换？
3. 加热器故障是否应升级为 EtherCAT 保护（当前是本地故障）？
4. 是否需要硬件看门狗（当前 `CONFIG_REBOOT=n`）纳入 Lifecycle？
5. 未来是否多板/多产品？这决定 `board/` 的资源表抽象深度。

**验收标准（新架构）**
- `dust_domain` 在无 Zephyr 环境可编译并通过全部单元测试；
- 分层 target 链接图无环、无越界 include；
- 同一 ABI golden 向量下，新旧固件 Publish/Process 结果逐字节一致；
- 故障注入下 safe-stop 顺序、幂等、诊断字段与现行为一致；
- 125 µs 台架 P99 满足预算，长稳 ≥ 24 h 无保护误触发；
- 代码中不存在 `gateway.cpp` 式单文件多职责、无 `io_backend` 式反向依赖、
  无未声明头文件的跨 TU `extern`。

---

## 20. 与现状相比的净变化

| 维度 | 现状 | 新架构 |
|---|---|---|
| 依赖方向 | include 单向，但存在 io↔ecat 链接环 | 严格 DAG，target 级强制 |
| 保护 | 32 处分散 `Protect` | 1 个 ProtectionService + 故障上报接口 |
| 状态 | Core + capturing + ready + rearm_pending 多源 | 1 个 LifecycleService 状态机 |
| SSC | 与业务同目录、630 行 gateway | 独立适配器、5 个小文件、薄回调 |
| 板级特性 | 通用层 `#if` + CMake `if` 混用 | board 资源表 + 组合根 |
| 常量来源 | 对象索引 3 份、PDO schema 未校验 | 生成物唯一来源 + 生成一致性测试 |
| 测试 | facade 级集成测试为主 | domain fake 端口单测 + adapter 集成 + 合约 |
| 可观测性 | 诊断限流打印 | 状态分页 + 事件环 + 周期耗时统计 |

---

## 21. 实施记录（2026-11-02，已一步到位落地）

新架构已在 `dust-adapter` 工作树中替换旧结构（未提交），验证结果：

| 验证项 | 结果 |
|---|---|
| 原生合约测试 | CTest **26/26 通过**（ASan/UBSan，含 SSC 适配器合约、IO 策略、身份、保护、发布等价） |
| 全量测试入口 | `./scripts/test_protocol.sh` **RESULT=0**（含外部 hpm_support 原生测试；macOS 下 UART idle 用例按既有规则 SKIP） |
| 固件构建 | `west build -b dust-hpm5e31` **成功**（`build/final`） |
| 资源 | ROM 196,960 B（23.1%）；RAM 215,904 B（82.4%）；相对旧固件 dec +25.8 KB，主要来自静态组合对象与抽象层（后续可优化） |
| 依赖边界 | `tools/check_driver_boundary.py` 通过；`dust_domain` 不依赖 Zephyr/SSC（仅链接 `zephyr_interface` 统一 stdint 配置） |
| 文档链接 | `tools/check_doc_links.py` 通过 |

**落地结构**：`src/{ports,domain,drivers,adapters,board,app}`；`domain` 15 个编译单元、`ports` 11 个接口头、`adapters/ethercat_ssc` 6 个文件（原 630 行 gateway 拆分）、`board/dust_hpm5e31` 单板资源表；CMake 改为 ports/domain/drivers/adapters 分层 OBJECT 库，生成物统一在构建目录。

**遗留优化项**：RAM 已用 82%，后续需做缓冲/对象瘦身与实时预算实测；加热器故障按决策保持本地维护；未引入硬件看门狗；ABI 5.0/线格式未改变。
