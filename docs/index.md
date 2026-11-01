> 状态：living · 更新：2026-11-02 · 分支：ethercat

# dust-adapter 文档导航

本文是仓库文档的唯一导航入口。各文档只描述当前代码与构建状态，
不保留历史实验记录、历史交接或迁移草稿。

## 文档约定

- 命名统一为小写 kebab-case；新增、重命名或删除文档时同步更新本页。
- 过期文档直接删除，由 `git log` 追溯历史，不设 archive 目录。
- 每篇 living 文档开头写一行状态元信息（状态、更新日期、分支）。
- 正文中的仓库相对路径与 Markdown 相对链接必须指向真实文件，
  由 `tools/check_doc_links.py` 校验；CI 在
  `.github/workflows/ethercat.yml` 中执行同一脚本。

## 入门

| 文档 | 说明 |
|---|---|
| [README](<../README.md>) | 项目简介、当前能力边界、构建与测试入口 |
| [源码架构](architecture.md) | 源码分层、线程边界、构建与测试入口 |
| [C++ 风格](cpp-style.md) | 长期 C++ 编码与维护约定 |

## 架构

| 文档 | 说明 |
|---|---|
| [源码架构](architecture.md) | `src/main.c`、`src/protocol`、`src/ecat`（含 `generated`/`appl_interfaces`/`esc_hw.*`）、`src/drivers` 的职责与数据流 |
| [EtherCAT 协议核心](ethercat-protocol-core.md) | 配置事务、状态机、授权期限、UART/反馈/诊断编码 |
| [PDO 协议](ethercat-pdo-protocol.md) | ABI 5.0 字段级线格式与布局签名 |

## EtherCAT 协议

| 文档 | 说明 |
|---|---|
| [EtherCAT 从站](ethercat.md) | SSC 5.13、FreeRun 当前能力、许可与构建入口 |
| [SSC 网关接入](ethercat-ssc-gateway.md) | 对象字典、SII/ESI、SM 分区、CAN/UART 后端、启动计数与诊断 |
| [EtherCAT 协议核心](ethercat-protocol-core.md) | 与 Zephyr/SSC 解耦的可原生测试协议核心 |
| [PDO 协议](ethercat-pdo-protocol.md) | 生产 profile 的 RxPDO/TxPDO 字段与语义 |

## 板级

| 文档 | 说明 |
|---|---|
| [HPM5E31 板级说明](hpm5e31-board.md) | 板型、PHY/CAN/UART 接线、JTAG 与 ESC 驱动接口 |
| [板载 IMU](onboard-imu.md) | HXY IMU 的 SPI/DMA 采样、原始数据与温度边界 |
| [CAN 配置](<../config/README.md>) | `config/can_devices.yaml` 白名单、保护帧与台架 profile |

## 测试与构建

| 文档 | 说明 |
|---|---|
| [源码架构](architecture.md) | 构建入口、生成文件与测试入口说明 |
| [README](<../README.md>) | `scripts/build.sh ethercat` 与 `scripts/test_protocol.sh` 用法 |

测试统一从 `scripts/test_protocol.sh` 进入，覆盖 `tests/` 下的原生合约测试；
硬件探针固件位于 `tests/`，需单独构建与烧录，联机前须确认 EtherCAT 主站不活动。
