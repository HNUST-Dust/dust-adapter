> 状态：living · 更新：2026-11-07 · 分支：ethercat

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

原接线 DTG1 直接交接版本已部署并完成一分钟实测，见 [直接交接报告](gateway-direct-ingress-2026-11-07.md)。DTG2 隔离试验因终端电阻约束取消，未部署。固定下行 60 B、上行 180 B，主站交换周期 125 µs。

- ESC PDI/SM 中断负责过程影像映射，下行六命令整批校验后分发物理 CAN lane。默认 DTG1 用 CAN0/CAN3 两个 lane；DTG2 隔离配置用 CAN0..3 四个 lane，路由及保护 YAML 同步切换。
- CAN TX worker 使用最高协作优先级，硬件消息 RAM 准备允许外部中断；最终 TXBAR 请求在驱动锁内检查停止、时效与新命令序号。
- CAN RX 回调更新最新完整反馈缓冲，InputMapping 合并快照，路由电机不再重复进入消息队列。
- 新候选在成功 SPI/DMA 回调中发布完整板载 IMU 紧凑页；优先级 0 的 IMU 线程只负责启动排队采集、事件等待及故障恢复。有界 UART ingress 完成 HI91 CRC/裁剪与完整页发布，取消独立 HI91 线程。
- 优先级 0 的 PDO owner 处理握手、看门狗、SBUS 块确认。Arm 应答保持至首批有效命令；SBUS 保留一个不可变待确认页，由 InputMapping 重试，不另建中间八块队列。
- 主循环优先级 4，处理生命周期与板级服务。默认 PLIC 非嵌套，IRQ 优先级仅决定待处理中断的选择顺序。
- 输出与新命令各有 20 ms 保护。Stop、传输错误和看门狗触发后，取消待发命令并发送配置保护帧。

正常配置不包含源到达统计。CONFIG_ECAT_SOURCE_ARRIVAL_STATS 为手动测量开关，默认 n；上位机到达间隔工具也只在手动启动时运行。

## 关键数据流

```text
下行：ESC RxPDO → OutputMapping → 整批解码和 CAN 白名单校验
     → 两侧最新命令缓冲 → 独立 CAN TX worker → MCAN 批量提交
上行 CAN：CAN RX 回调 → 最新完整反馈缓冲 → InputMapping → ESC
上行 IMU：DRDY → 采集启动线程 → DMA 完成回调 → 完整页交接 → InputMapping
上行 HI91：UART DMA/idle → 有界接收解析回调 → 完整页交接 → InputMapping
上行 SBUS：UART DMA → PDO owner → 块确认/重试 → InputMapping
保护：Stop / 超时 / 传输错误 → 取消发送 → YAML 配置保护帧
```

固件没有运行时测试计数、时序跟踪或到达间隔探针。到达间隔由手动启动的上位机工具测量，正常运行不启动该工具。

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

当前部署状态与未完成的 CAN 隔离验证以 [优先级优化记录](gateway-dtg1-optimization-2026-11-07.md) 为准。
