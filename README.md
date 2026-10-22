# dust-adapter

Dust MCU 通讯适配固件。独立于机器人控制算法，分别构建 USB 网关和 EtherCAT 从站。

| 应用 | 板卡 | 当前功能 |
|---|---|---|
| `apps/usb_gateway` | dust-hpm6750 | 已验证 USB 网关：CAN、SBUS UART、HI91 UART、板载原始 IMU 转发 |
| `apps/ethercat_slave` | dust-hpm5e3y | 现有 SSC 5.13 FreeRun 从站基础：计数器 PDO、ESC/PHY 与 SII |

**EtherCAT 目前不是 CAN/UART/IMU 网关**，也不是 USB 协议的 EtherCAT 实现。
本次按确认范围迁出现有从站基础，不带底盘 MPC/控制器，不新增转发 PDO。

## 目录

- `apps/`：两个独立 Zephyr 应用入口与各自板级配置。
- `drivers/`、`include/dust_adapter/`：CAN 调度、USB 会话、板载 IMU 等硬件访问。
- `protocols/`、`include/protocols/`：传输协议、必要的电机编解码。
- `config/`、`interfaces/`、`tools/`：配置值与消息类型生成；没有底盘算法。
- `patches/`：尚未合入 HPM 支持库的 UART10 正式功能补丁。
- `docs/`：协议、依赖、迁移及验证说明。

## 新建 workspace

需要 Python3/West/PyYAML、Zephyr 对应工具链、CMake/Ninja。依赖已在 `west.yml`
固定版本，不需要克隆 `wbr_control` 或 `wbr_control_companion`：

```bash
mkdir dust-workspace
cd dust-workspace
west init -m https://github.com/HNUST-Dust/dust-adapter.git --mr main
west update
west zephyr-export
```

USB 所需的 UART10 修复尚以补丁提供。在干净的固定 HPM 支持版本中应用一次：

```bash
git -C hpm_support apply --check ../dust-adapter/patches/hpm_uart_line_idle.patch
git -C hpm_support apply ../dust-adapter/patches/hpm_uart_line_idle.patch
```

若已含补丁，`git apply --reverse --check` 可确认，不能盲目重复应用。

## 构建 USB

```bash
./dust-adapter/scripts/build.sh usb
```

默认 SDK 位于 workspace 内的 `sdk_env/hpm_sdk`。复用其他布局可设置
`HPM_SDK_DIR` 环境变量；`DUST_HPM_SUPPORT_DIR` 是可选 CMake 参数。

```bash
HPM_SDK_DIR=/path/to/hpm_sdk ./dust-adapter/scripts/build.sh usb
# 只在准备部署到板子时手动刷写：
west flash -d dust-adapter/build/usb_gateway
```

USB 保留现有 NUC 兼容协议、VID/PID、Bulk 128B OUT/64B IN、W/B/D 批次顺序与
20ms 命令租约。配置目前对应六电机，命令地址有白名单，不是任意 CAN 帧隧道。
详见 [USB 网关](docs/usb.md)。

## 构建 EtherCAT

先准备已有许可和匹配配置的 SSC 5.13 生成源码，然后本地导入：

```bash
python3 dust-adapter/tools/import_ssc.py /path/to/ssc_generated/src
./dust-adapter/scripts/build.sh ethercat
```

也可在构建时指定 `-- -DDUST_ADAPTER_SSC_DIR=/path/to/ssc_generated/src`。
SSC 源码位于忽略目录 `external/ssc/`，不随本仓库发布；原始许可证适用。
没有匹配的 SSC 文件时配置会明确失败，不会编出假从站。
ESI、PDO 与 SSC 配置要求见 [EtherCAT 基础](docs/ethercat.md)。

## 迁移边界与验证

两应用均已在原有 Zephyr/HPM 依赖环境下独立编译，无需原控制工程目录。
USB 迁出前后在相同驱动基线下已做 `.bin` 字节一致性核对；两个应用也已通过固定版本依赖加 UART 补丁的独立构建，未带入 CAN 实验追踪改动。详情见 `docs/build_verification.json`。这些检查不替代新目录部署后的硬件回归。
没有刷写硬件或启动电机。EtherCAT 保持联调基础状态，不宣称正式 CAN 桥已完成。

旧控制工程仅保留迁移指引；历史源码仍可在其 Git 版本中查阅。
本仓库不包含 LQR/VMC/MPC、ROS 2、Foxglove、压力测试或现场采集程序。
