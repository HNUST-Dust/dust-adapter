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

先准备已有许可和匹配配置的 SSC 5.13 生成源码，然后本地导入：

```bash
python3 dust-adapter/tools/import_ssc.py /path/to/ssc_generated/src
./dust-adapter/scripts/build.sh ethercat
```

也可在构建时指定 `-- -DDUST_ADAPTER_SSC_DIR=/path/to/ssc_generated/src`。
SSC 源码位于忽略目录 `external/ssc/`，不随本仓库发布；原始许可证适用。
没有匹配的 SSC 文件时配置会明确失败，不会编出假从站。
ESI、PDO 与 SSC 配置要求见 [EtherCAT 基础](docs/ethercat.md)。

## CI 与授权

GitHub Actions 检查脚本语法和 ESI XML。公共 CI 不包含 SSC，不声称完成固件编译。
完整构建请在持有合法 SSC 5.13 配置源码的环境执行上述命令，禁止上传授权受限源码
作为公共 CI 产物或提交。当前不自动发布 EtherCAT 二进制。

拆分前已通过独立构建，记录见 `docs/build_verification.json`；未刷写硬件。
