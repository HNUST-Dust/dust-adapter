# dust-adapter — USB gateway

`main` 是当前正式 USB 方案，`usb` 是 USB 开发/发布分支；`ethercat` 独立维护
HPM5E3Y SSC FreeRun 从站基础。`v0.1` 为 USB 正式版本。

本分支只包含 dust-hpm6750 的 USB Bulk → CAN/UART/IMU 固件，无底盘算法。
EtherCAT 源码、构建和 SSC 导入说明请切换到 [ethercat 分支](https://github.com/HNUST-Dust/dust-adapter/tree/ethercat)。

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

## CI 与版本发布

Push/PR 到 `main` 或 `usb` 自动构建 dust-hpm6750 USB 固件。`v*` 标签在构建
成功后自动生成 GitHub Release，附带 BIN、ELF、SHA256 和完整构建配置/依赖清单。
不自动刷写硬件。依赖固定于 `west.yml`，CI 使用 Zephyr SDK 0.16.8。

```bash
git tag -a v0.1 -m "USB gateway v0.1"
git push origin v0.1
```

详细行为和失联策略见 [USB 网关](docs/usb.md)，迁移记录见 [迁移说明](docs/migration.md)。
历史同驱动基线下的二进制一致性核对已完成；固定依赖构建通过。新版本仍需按板卡
部署流程验证，CI 构建成功不代表已通过硬件回归。
