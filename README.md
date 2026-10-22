# dust-adapter — EtherCAT slave foundation

`ethercat` 分支独立维护 dust-hpm5e3y 的 SSC 5.13 FreeRun 从站基础。
USB 正式方案位于 [main](https://github.com/HNUST-Dust/dust-adapter/tree/main) 和
[usb](https://github.com/HNUST-Dust/dust-adapter/tree/usb) 分支，`v0.1` 属于 USB 方案。

本分支只有计数器 PDO、ESC/PHY 与 SII 基础，不是 CAN/UART/IMU 转发网关，也没有底盘算法。

## 初始化依赖

```bash
mkdir dust-workspace && cd dust-workspace
west init -m https://github.com/HNUST-Dust/dust-adapter.git --mr ethercat
west update
west zephyr-export
```

需要 Python3/West、CMake/Ninja 和 Zephyr 对应工具链，依赖版本固定于 `west.yml`。

## 构建 EtherCAT

配置后的 SSC 5.13 已随 `external/ssc` 纳入本分支，直接构建：

```bash
./dust-adapter/scripts/build.sh ethercat
```

仍可通过 `-- -DDUST_ADAPTER_SSC_DIR=/path/to/ssc_generated/src` 显式选用其他匹配配置。
ESI、PDO 与 SSC 配置要求见 [EtherCAT 基础](docs/ethercat.md)。

## CI 与授权

Push/PR 到 `ethercat` 会完整编译 dust-hpm5e3y 从站，并上传 BIN、ELF、ESI、构建配置、
依赖清单和校验值。CI 不自动刷写硬件，也不会创建 USB 的版本标签。

SSC 按维护者确认的芯片厂商授权范围纳入，保留 Beckhoff 原始许可；不适用本仓库
Apache-2.0。详见 `external/ssc/README.md`。本分支仍是计数器 PDO 从站基础。
