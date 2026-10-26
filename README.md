# dust-adapter — EtherCAT development on the generic USB baseline

`ethercat` 已合入 `feature/generic-usb-adapter`，以其 CAN/UART 驱动、编译期 YAML
白名单与保护、原始 IMU、温控及测试为后续 HPM5E31 网关固件的基础。
USB 基线保留在根目录 `src/`，旧 EtherCAT 从站保留在 `apps/ethercat_slave/`。

**当前 EtherCAT 应用仍是 SSC FreeRun 计数器从站，不是新主机所需的 CAN/UART 网关。**
主机动态 PDO、0x2100 启动配置、会话与保护协议将在此基础上逐步实现，未宣称接板可用。

## 构建

在已初始化的 West 工作区中，安装相应 Zephyr SDK 后：

```bash
./dust-adapter/scripts/build.sh ethercat -p always
# USB 基线需要显式选择：
./dust-adapter/scripts/build.sh usb -p always
./dust-adapter/scripts/test_protocol.sh
```

不带目标参数时默认 ethercat。默认输出分别为 build/ethercat_slave、build/usb_gateway，
避免覆盖旧 build 缓存；可用 BUILD_DIR 指定独立目录，WEST 指定 west 可执行文件。
EtherCAT 默认板为 dust-hpm5e31，按 HPM5E31.pdf 原理图独立定义，使用两颗外置 PHY。
SPI3 IMU 与加热暂时关闭；接线与待验证项见 [板级迁移说明](docs/HPM5E31_BOARD.md)。

## 开发边界

- 根目录 USB 应用与其固件协议保持基线，先复用 CAN/UART 及本地保护，不迁入主机控制算法。
- apps/ethercat_slave 保留片上 ESC/PHY、SSC 5.13、SII 和旧计数器 ESI。
- 原 ESI 的实验 Vendor ID=0、计数器 PDO 不能用于新主机动态网关。
- 依赖由 west.yml 固定，新板定义在 hpm_support；SDK 保持原版本。
- 不自动刷写、使能电机、启动机器人或烧写 OTP。编译通过不等于硬件验收。

[旧 EtherCAT 基础](docs/ethercat.md)、[USB generic 基线](docs/USB_GENERIC_BASELINE.md)、
[CAN YAML 配置](config/README.md)、[维护风格](docs/CPP_STYLE_HANDOFF.md)。
SSC 按原许可与维护者授权记录保留，见 external/ssc/README.md；不重新许可为 Apache-2.0。

开发顺序与主机协议基线见 [HPM5E31 网关起点](docs/HPM5E31_GATEWAY_BASELINE.md)。
