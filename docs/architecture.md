> 状态：living · 更新：2026-11-02 · 分支：ethercat

# 源码组织、线程边界与构建入口

本文描述当前仓库的固件分层、执行上下文和构建/测试入口。
协议细节见 [EtherCAT 协议核心](ethercat-protocol-core.md) 与
[PDO 协议](ethercat-pdo-protocol.md)，SSC 接入细节见
[SSC 网关接入](ethercat-ssc-gateway.md)。

## 单一构建入口

仓库只有一个固件构建入口：根目录 `CMakeLists.txt` + `prj.conf`，板级覆盖在
`boards/`，工作区/依赖路径由 `cmake/prepare.cmake` 准备。
`scripts/build.sh ethercat` 只是 `west build -b dust-hpm5e31` 的封装；
`src/ecat/generated` 的授权源码与 `src/ecat/sampleappl.h` 由 CMake 复制到构建目录
`build/<dir>/ssc_gateway` 后编译，不改写第三方源文件。

构建期生成的文件都落在构建目录，不提交：

- `build/<dir>/generated/can_profile_generated.hpp`：由
  `tools/generate_can_profile.py` 从 `config/can_devices.yaml` 生成。
- `build/<dir>/ssc_gateway/`：SSC 源码副本、`sampleappl.h`、`gateway_config.h`、
  SII 镜像与 SSC 宏定义。

随仓库提交的生成/派生文件：

- `src/protocol/pdo_schema.hpp`：由 `tools/generate_pdo_schema.py` 从
  `config/pdo_schema.yaml` 生成，固件与主机共用同一份字段定义，不手改。
- `src/ecat/generated/`：SSC 5.13 工具导出的配置后工程源码与授权栈实现，
  构建期不重新生成，只能通过 `tools/import_ssc.py` 受控更新（详见其 README）。
- `config/esi/DUST_HPM5E31_Gateway.xml`：由
  `tools/generate_gateway_esi.py` 从 `config/ethercat_gateway.json` 生成并归档，
  与构建期 SII 同源。

## 源码分层

| 路径 | 职责 |
|---|---|
| `src/main.c` | 板级外设初始化、主循环调度策略；生命周期由 `src/ecat/runtime.c` 承担 |
| `src/protocol/` | 与 Zephyr、SSC、外设解耦的协议核心与布局，可原生编译测试 |
| `src/protocol/protocol.hpp` | ABI 常量、配置/布局/Layout/Actions/Core 接口 |
| `src/protocol/core.cpp` | 配置提交、状态机、会话与过期判据、UART 窗口、PDO 编解码 |
| `src/protocol/layout.cpp` | 由配置与 Limits 生成候选 PDO 布局 |
| `src/protocol/pdo_dictionary.*` | CoE 映射暂存器、过程 RAM 分配与冻结 |
| `src/protocol/digest.hpp` | 布局/影像摘要 |
| `src/protocol/io_policy.hpp` | 无堆的有界队列、命令过期与启动身份推导 |
| `src/protocol/pdo_schema.hpp` | 由 schema 生成的字段偏移与 Walk（勿手改） |
| `src/io/io_backend.cpp` + `io_backend.hpp` | IO facade：初始化、保护、配置、状态查询、动作派发与跨周期采集 |
| `src/io/can_adapter.cpp` | CAN 适配层：Core 事件、接收队列、批次 deadline、保护帧、反馈快照与状态 |
| `src/io/uart_adapter.cpp` | UART 适配层：接收队列、epoch/窗口、超时、状态编码与快照 |
| `src/io/can_adapter.hpp` | CAN 适配层对外接口（状态由 can_adapter.cpp 私有） |
| `src/io/uart_adapter.hpp` | UART 适配层对外接口（状态由 uart_adapter.cpp 私有） |
| `src/io/can_profile.hpp` | 编译期 CAN 白名单与保护帧策略 |
| `src/io/latest_can.hpp` | 每总线按 ID 的最新反馈槽 |
| `src/ecat/gateway.cpp` | 网关：CoE 对象字典、SSC `APPL_*` 回调、Core 生命周期与周期 poll |
| `src/ecat/runtime.c` + `runtime.h` | SSC/网关生命周期（启动、reset、rearm、poll/MainLoop 步进）与 `stack_ready` 状态 |
| `src/ecat/boot_identity.cpp` | NVS 启动身份读取/递增与读回 |
| `src/ecat/sampleappl.h` | 自定义 SSC 应用接口头，构建时替换授权包内的同名文件 |
| `src/ecat/esc/esc_hw.h` | SSC `HW_*` 契约头与 ESC 启动/启停 API 声明 |
| `src/ecat/esc/esc_regs.c` | ESC 寄存器读写、定时器、AL event、临界区、IRQ 与 EEPROM 命令循环 |
| `src/ecat/esc/sii.c` + `sii.h` | SII 镜像存储与 EEPROM 读/写/reload 响应 |
| `src/ecat/esc/esc_boot.c` | ESC/PHY/SII 启动、FreeRun 启停、链路维护与 PDI/RESET 中断钩子 |
| `src/drivers/` | 板级驱动：PWM、蜂鸣器、IMU、加热 PID，以及与 EtherCAT 无关的硬件层 |
| `src/drivers/can_driver.*` | CAN 硬件驱动：设备/位率/过滤器、原始帧收发、abort 与状态 |
| `src/drivers/uart_driver.*` | UART 硬件驱动：端口配置、接收缓冲/idle、原始块收发与 abort；`uart_receive.*` 为其内部实现 |
| `src/ecat/generated/` | 已授权的 SSC 5.13 配置生成源码与许可，不适用 Apache-2.0；`src/ecat/sampleappl.h` 是本仓库自有的 SSC 应用接口头，二者职责不同 |

## 启动与主循环

`src/main.c` 只负责板级外设初始化、启动/停止调用和主循环调度策略；ESC/PHY/SII
启动在 `src/ecat/esc/esc_boot.c`，SSC 生命周期在 `src/ecat/runtime.c`。

1. `ecat_runtime_init()` 注册 ESC PDI/RESET 钩子，`esc_init()` 完成
   SII 镜像装载、EEPROM 模拟配置与装载检查、PHY 复位/配置、信号配置和链路更新。
2. 蜂鸣器初始化，`ecat_runtime_start()` 调用 `MainInit()` 与
   `gateway_init()`（IO 后端、启动身份和协议核心对象字典）。
3. 板级输出（IMU 加热 PID）初始化，`ecat_runtime_enable()` 置位
   `stack_ready` 并启动 ESC（关闭 FreeRun 不需要的 SYNC0/SYNC1 中断）。
4. 进入 `for (;;)` 主循环：`ecat_runtime_step()` 处理 reset/rearm、SSC
   `MainLoop()` 前后各一次 `gateway_poll()`、每 100 ms 更新 PHY 链路，并
   返回 Active/Idle/Fatal；主循环据此选择 10 µs 忙等或 `k_usleep(100)`。

会话结束（`APPL_StopInputHandler`）后核心处于 Protected，主循环在屏蔽 ESC IRQ
的安全上下文中重建网关，产生新的 boot_id 并清空映射/判据/过程影像，使主站下一次
启动可直接重新配置 0x2100，无需断电或 ESC 复位；重建失败时保持 IRQ 屏蔽并停住主循环。

## 执行上下文

当前选择 FreeRun + **单 SSC 主线程**：主线程（`CONFIG_MAIN_THREAD_PRIORITY=0`）
唯一拥有 SSC、协议核心、软件队列、UART 窗口和 ESC 寄存器访问。

| 上下文 | 职责 | 边界 |
|---|---|---|
| 主线程 | SSC `MainLoop()`、PDO 回调、`Core.Process/Publish/Tick`、CAN/UART 队列排空与硬件提交 | 不把 Core 或 SSC 调用放进 ISR/工作线程 |
| ESC PDI/RESET IRQ | 处理 EEPROM 模拟、把 PDI 通知延迟给主线程 | 只做有界工作，不在 ISR 内解析业务协议 |
| CAN IRQ | 接收帧投递到每总线 `k_msgq`，主线程排空 | ISR 不直接调用 Core/SSC，不发送帧 |
| UART 中断/DMA 完成 | idle 检测与异步收发回调，把完整块/状态入队 | 不覆盖在途缓冲区，不推进 ACK |
| 系统工作队列 | IMU 加热 PID 周期工作（默认 10 ms）与板级输出 | 通过互斥区获取最新温度，不访问 ESC/过程数据 |

FreeRun 下不使用 SM/DC 同步中断；SYNC0/SYNC1 中断在启动后关闭。默认打开的若干
编译期优化开关（跳过多余覆盖清零、输入映射前刷新一次 CAN 反馈、网关 AL 状态诊断等）
都只在主线程生效，不改变 wire ABI。

## 数据流

- 下行：主站 RxPDO → SSC `APPL_OutputMapping` 复制到 `rx_image` →
  `APPL_Application` 调用 `Core.Process()` → `Actions` 交给 `io_backend`
  的 CAN 批次/UART 交付 → 硬件提交。CAN/UART 中断只把接收结果入队。
- 上行：主线程采集 CAN 反馈槽、UART 上行块与 IMU 最新突发 →
  `Core.Publish()` 编码 `tx_image` → SSC `APPL_InputMapping` 复制到 ESC 输入区。
- 本地保护：`io_backend` 的校验回调与停机回调在 `Core` 构造时安装，
  保护就绪后才声明 capability bit31；保护帧、命令期限和最终提交检查均在
  `io_backend` 内完成。

## 测试入口

统一入口：

```sh
./scripts/test_protocol.sh
```

该脚本先通过 `tests/CMakeLists.txt` 构建并运行 CTest 原生合约测试
（协议/布局/PDO 字典/IO/SSC/驱动边界、生成配置一致性），再运行锁定的
`hpm_support` 外部原生测试；外部脚本不可用时明确 SKIP，真实失败让总入口返回非 0。
需要与主机布局交叉检查时设置 `HOST_ROOT`。
硬件探针固件位于 `tests/`，不属于该入口，需单独 `west build` 并烧录；
联机测试前须确认 EtherCAT 主站不活动。
固件构建验证：

```sh
./scripts/build.sh ethercat -p always
```

## 工作区依赖

`west.yml` 固定 Zephyr（hpmicro fork）、`hpm_support` 与 `sdk_env` 版本；
`hpm_support` 提供板级 DTS、HPM_ESC 驱动和 PHY 初始化。修改依赖版本需要
同步验证构建与板级行为。
