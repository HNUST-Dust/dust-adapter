> 状态：living · 更新：2026-11-02 · 分支：ethercat

# dust-adapter — EtherCAT CAN/UART 网关

本仓库是运行在 HPM5E31 上的 EtherCAT 从站固件。从站只负责过程数据交换、
CAN/UART 外设收发与本地保护；控制算法留在主机（dust-control）。

## 当前能力边界

- 片上 ESC + 两颗外置 PHY（RTL8201F/TJA1101B），SSC 5.13，**FreeRun** 模式，
  单 SSC 主线程拥有全部 ESC 与过程数据访问。
- 动态 PDO：主站通过 0x2100 配置和 CoE 映射下载决定启用端口与容量，
  线格式见 [PDO 协议](docs/ethercat-pdo-protocol.md)。
- CAN 与 UART 后端已接入：CAN 批次收发、UART 原生 idle/DMA 收发、
  MCU 侧白名单与保护帧、本地停机回调；保护就绪后对外声明 capability bit31 并允许 Arm。
- 板载 IMU（SPI3 + XDMA）采样、蜂鸣器与 IMU 加热 PID 已启用；
  板级接线见 [HPM5E31 板级说明](docs/hpm5e31-board.md)。
- 实验身份 VID=0x0d57、PID=0x44544331、Revision=0x00010000，仅用于台架，
  不应作为生产身份使用。构建与离线测试通过不代表实板验收。

## 构建

在已初始化的 West 工作区中执行：

```sh
cd dust-adapter
./scripts/build.sh ethercat -p always
# 等价入口，默认输出目录 build/ethercat_slave：
west build -b dust-hpm5e31 -p always
```

`scripts/build.sh` 只构建、不刷写；输出目录可用 `BUILD_DIR` 指定。
CAN 白名单与保护帧由 `config/can_devices.yaml` 编译进固件，
可选台架 profile 位于 `config/labs/`，通过
`-DADAPTER_CAN_CONFIG=/绝对路径/config.yaml` 指定。
目录与线程职责见 [源码架构](docs/architecture.md)，
SSC 配置与许可见 [EtherCAT 从站](docs/ethercat.md)。

## 构建检查

CI 执行文档链接检查、依赖边界检查与目标固件构建。仓库不再维护宿主机测试；
功能正确性以目标构建和实板台架验收为准。
硬件探针固件位于 `tests/probes/`，需单独 `west build` 并烧录；
联机前须确认 EtherCAT 主站不活动。

## 目录结构

```text
dust-adapter/
├── CMakeLists.txt        # 分层静态库与唯一固件构建入口
├── Kconfig               # 可选功能开关
├── prj.conf              # 默认 EtherCAT 应用配置
├── boards/               # 板级 overlay
├── cmake/                # 工作区与依赖路径准备
├── config/               # 编译期配置：CAN 设备、PDO schema、网关身份、ESI
│   └── labs/             # 台架 CAN profile
├── docs/                 # living 文档
├── scripts/              # 构建与测试入口
├── src/
│   ├── main.cpp           # 入口
│   ├── composition.*      # 组合根
│   ├── scheduler.*        # 实时循环
│   ├── common/           # 跨层值类型（Error/Bytes/CAN/UART/IMU/故障钩子）
│   ├── domain/           # protocol（会话/线协议/CAN/UART 子协议）、pdo、can、uart、protection
│   ├── drivers/          # 窄硬件原语 + CAN/UART/IMU 端口实现
│   ├── adapters/
│   │   ├── esc/          # ESC 寄存器/SII/PHY/启动
│   │   └── ethercat_ssc/ # 唯一 include SSC 的适配器（回调/对象字典/映射）
│   └── ssc_generated/    # 授权 SSC 5.13 源码（非 Apache-2.0）
├── tests/probes/         # 硬件探针固件（非宿主机测试）
├── tools/                # 配置/ESI/schema 生成与依赖边界检查
└── west.yml              # West manifest
```

依赖方向：`common → domain → drivers/adapters → app`；domain 按 protocol/pdo/can/uart/protection 划分；
单板资源/DTS 别名集中在 `composition.cpp`。
越界 include 由 `tools/check_driver_boundary.py` 校验，主约束是 CMake target 链接图。

## 文档

文档导航入口：[docs/index.md](docs/index.md)。
文档一律使用小写 kebab-case 命名，过期文档直接删除而不设 archive；
文中引用的仓库路径由 `tools/check_doc_links.py` 校验，CI 见
`.github/workflows/ethercat.yml`。

## 许可

除第三方组件外，本仓库采用 Apache-2.0（见 `LICENSE`、`NOTICE`）。
`src/ssc_generated` 为已获授权的 Beckhoff SSC 5.13，**不适用** Apache-2.0，
许可与导入说明见 [SSC 许可](src/ssc_generated/README.md)。
