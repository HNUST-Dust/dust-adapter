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

## 测试

```sh
./scripts/test_protocol.sh
```

该入口通过 `tests/CMakeLists.txt` + CTest 运行原生协议/布局/IO/SSC 合约测试与
配置生成一致性检查，再运行锁定的 `hpm_support` 外部原生测试（不可用时明确 SKIP）。
硬件探针固件位于 `tests/`，不在此入口内，需单独 `west build` 并烧录；
联机前须确认 EtherCAT 主站不活动。

## 目录结构

```text
dust-adapter/
├── CMakeLists.txt        # 唯一固件构建入口
├── Kconfig               # 可选功能开关
├── prj.conf              # 默认 EtherCAT 应用配置
├── boards/               # 板级 overlay
├── cmake/                # 工作区与依赖路径准备
├── config/               # 编译期配置：CAN 设备、PDO schema、网关身份、ESI
│   └── labs/             # 台架 CAN profile
├── docs/                 # living 文档
├── scripts/              # 构建与测试入口
├── src/
│   ├── main.c            # 板级初始化与主循环调度
│   ├── protocol/         # 可原生测试的协议核心与 PDO 布局
│   ├── io/               # CAN/UART 协议适配与 IO facade（依赖 protocol + drivers）
│   │   ├── io_backend.cpp/hpp   # IO facade
│   │   ├── can_adapter.cpp      # CAN 适配层
│   │   ├── uart_adapter.cpp     # UART 适配层
│   │   ├── can_adapter.hpp      # CAN 适配接口
│   │   ├── uart_adapter.hpp     # UART 适配接口
│   │   ├── io_common.hpp        # 时间辅助
│   │   ├── can_profile.hpp      # CAN 白名单/保护帧策略
│   │   └── latest_can.hpp       # 每总线最新反馈槽
│   ├── ecat/             # SSC 网关、ESC 硬件适配与授权 SSC 源码
│   │   ├── gateway.cpp/h        # 对象字典 + SSC 回调 + 生命周期
│   │   ├── runtime.c/h          # SSC/网关生命周期
│   │   ├── boot_identity.cpp    # 启动身份
│   │   ├── sampleappl.h         # 自定义 SSC 应用接口头
│   │   ├── esc/                 # ESC 寄存器/SII/启动与 PHY 适配
│   │   └── generated/           # SSC 工具导出源码（非 Apache-2.0）
│   └── drivers/          # 板级与硬件驱动
│       ├── can_driver.*  # CAN 硬件驱动（无协议依赖）
│       ├── uart_driver.* # UART 硬件驱动（无协议依赖）
│       └── ...           # PWM、蜂鸣器、IMU、加热等
├── tests/                # 原生合约测试与硬件探针固件
├── tools/                # 配置/ESI/schema 生成与文档检查
└── west.yml              # West manifest
```

## 文档

文档导航入口：[docs/index.md](docs/index.md)。
文档一律使用小写 kebab-case 命名，过期文档直接删除而不设 archive；
文中引用的仓库路径由 `tools/check_doc_links.py` 校验，CI 见
`.github/workflows/ethercat.yml`。

## 许可

除第三方组件外，本仓库采用 Apache-2.0（见 `LICENSE`、`NOTICE`）。
`src/ecat/generated` 为已获授权的 Beckhoff SSC 5.13，**不适用** Apache-2.0，
许可与导入说明见 [SSC 许可](src/ecat/generated/README.md)。
