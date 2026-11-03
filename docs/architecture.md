> 状态：living · 更新：2026-11-02 · 分支：ethercat

# 源码组织、依赖方向与执行模型

本文描述 v2 固件架构的分层、依赖规则、执行上下文与构建/测试入口。
协议细节见 [EtherCAT 协议核心](ethercat-protocol-core.md) 与
[PDO 协议](ethercat-pdo-protocol.md)，SSC 接入见
[SSC 网关接入](ethercat-ssc-gateway.md)。

## 设计目标

- **低耦合**：共享值类型在 `common/`；SSC 只在 `adapters/ethercat_ssc`，
  Zephyr 外设访问集中在 `drivers/`，单板配置集中在 `composition.cpp`。
- **高内聚**：唯一生命周期状态机、唯一保护/停机所有者、单一对象索引事实源。
- **单板单产品**：不为多板/宿主机测试引入额外抽象层。

## 分层与依赖

| 路径 | 职责 | 允许依赖 |
|---|---|---|
| `src/common/` | 跨层值类型（Error/Bytes/CAN/UART/IMU/故障钩子/栈事件） | 仅标准库 |
| `src/domain/` | `protocol/`（会话、线协议、CAN/UART 子协议）、`pdo/`（配置/布局/映射）、`can/`、`uart/`、`protection/` | `common/`、`drivers/` |
| `src/drivers/` | 窄硬件原语 + CAN/UART/IMU 设备类（ISR 入队、批次/块提交） | `common/`、Zephyr/HPM SDK |
| `src/adapters/esc/` | ESC 寄存器/SII/PHY/启动 | Zephyr/HPM SDK |

> 注：授权 SSC 的 `ecatslv.h` 由 `tools/import_ssc.py` 在导入时把 ESC 硬件头引用
> 统一改为 `adapters/esc/esc_hw.h`（仓库唯一的 ESC 硬件契约头），因此不需要
> 额外的兼容头文件。
| `src/adapters/ethercat_ssc/` | SSC 适配器（唯一 include SSC 的地方）：生命周期、回调翻译、对象字典、PDO 暂存、IO 编排、反馈聚合、发布调度 | `common/`、`domain/`、SSC |
| `src/ssc_generated/` | 授权 SSC 5.13 配置后源码（非 Apache-2.0，仅导入工具更新） | 无（仅构建期复制） |
| `src/`（main/composition/scheduler/lifecycle/identity） | 组合根（单板资源/DTS 别名）、启动身份、实时循环、生命周期、main | 以上全部 + 生成头 |

CMake 目标：`dust_domain`（链接 Zephyr 接口）；`dust_drivers`（硬件原语 +
设备类）与 `dust_adapters`（ESC/SSC）→ `app`。越界 include 由
`tools/check_driver_boundary.py` 兜底。

## 执行模型

- **单实时主线程**（prio 0）拥有 Session、SSC、IO 队列、UART 窗口与 ESC 访问；
  125 µs 周期下不加锁、不阻塞、不打印、不分配堆。
- **ISR 只入队**：CAN RX、UART idle/DMA、IMU DRDY、ESC PDI/RESET 只写 SPSC 有界环
  或置位事件标志；不调用 domain 服务。
- **板级输出**由调度器每周期调用（适配器内部按 `period_ms` 限流），
  不依赖系统工作队列；加热 PID 与主站是否发布无关。
- **状态机唯一**：`Boot → Configurable → Ready → Active → Protected → Rearm`；
  AL 状态是输入，IRQ 门控是输出。

## 关键数据流

```
下行：ESC RxPDO → SSC APPL_OutputMapping → SscStack 调 Session.Process
     → Actions → IoCoordinator.Dispatch → CanIoService/UartIoService
     → drivers::CanBusPort/UartPort → 硬件
上行：硬件 ISR → 端口环 → CanIoService.DrainReceive/UartIoService.Capture
     → FeedbackAggregator → Session.Publish → tx_image
     → SSC APPL_InputMapping → ESC
保护：服务上报 FaultSink → ProtectionService 锁存 → Session.Protect
     → 固定顺序 safe-stop（UART abort、CAN 撤销+保护帧）；
       Protected 时调度器将加热器置安全状态
rearm：会话结束 → LifecycleService 在安全上下文 Stop/重新生成 boot_id/Start/Enable
```

## 构建期生成

| 生成物 | 来源 | 使用方 |
|---|---|---|
| `build/<dir>/generated/pdo_schema.hpp` | `config/pdo_schema.yaml` | `domain/pdo`、`domain/protocol` |
| `build/<dir>/generated/can_profile_generated.hpp` | `config/can_devices.yaml` | `composition` → `CanTopology` |
| `build/<dir>/ssc_gateway/*` | SSC 授权源码 + `config/ethercat_gateway.json` | `adapters/ethercat_ssc` |

生成物只落在构建目录，不提交；对象索引/区域常量只从生成头取。

## 构建检查

CI 执行文档链接检查、依赖边界检查与目标固件构建：
`tools/check_doc_links.py`、`tools/check_driver_boundary.py`、`west build`。
硬件探针固件位于 `tests/probes`，需单独构建烧录。

## 工作区依赖

`west.yml` 固定 Zephyr（hpmicro fork）、`hpm_support` 与 `sdk_env` 版本；
`hpm_support` 提供板级 DTS、HPM_ESC 驱动和 PHY 初始化。
