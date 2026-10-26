# HPM5E31 原理图板级迁移

默认构建 `./scripts/build.sh ethercat -p always` 选择 `dust-hpm5e31`。
在仓库根目录直接执行 `west build -b dust-hpm5e31 -p always` 也会加载同一
EtherCAT 应用及其 prj.conf，默认输出到 `build`；不会编译根目录 USB 应用。

板级定义位于工作区 hpm_support 的 `boards/hpmicro/dust-hpm5e31`，由 west.yml
固定版本。[引脚表与原理图待确认项](https://github.com/HNUST-Dust/hpm_support/blob/6a920426cce52a3d50650d1cfe1d3e63da1c3691/boards/hpmicro/dust-hpm5e31/README.md)。

## 本次落实

- 新建独立板型，不修改 dust-hpm5e3y 的接线。
- 按原理图接入四路 CAN、UART0～6、RTL8201F/TJA1101B 的 MII、共享 MDIO/MDC
  和 REFCK；TJA1101B 使用 PF28 复位、PF26 中断。
- 不再对新板执行旧 SIP PHY 的 PV/PW、PA28 CTR1 和模拟焊盘实验初始化。
  旧配置移入 `apps/ethercat_slave/boards/dust-hpm5e3y.overlay`，仅旧板构建加载。
- `prj.conf` 明确 `CONFIG_SPI=n`，SPI3 节点 disabled，暂不采集板载 IMU。
  CS PC21 默认高，IMU 加热门极 PD04 默认低，PWM 引脚仅启用蜂鸣器 PF01。
  外部 UART IMU 并未因此禁用。
- 原理图 WS2812 的 `PWM0_P2` 未连到 MCU，暂不启用，等待硬件接线确认。

## 验证及边界

新板和旧参考板的 EtherCAT SSC 应用均完成干净构建，未刷写硬件。
新板 ROM 94016 B、主 RAM 78408 B；旧参考板 ROM 96456 B、主 RAM 78408 B。
构建配置已核对为 HPM5E31、dust-hpm5e31、SPI 关闭，新板无 SPI 驱动目标。
新板仍使用旧 FreeRun 计数器 PDO；本次没有实现新的 CAN/UART 网关对象字典，
不能直接与主机动态网关 PDO 配套运行。

PHY 地址先按电阻配置使用 RTL8201F=1、TJA1101B=0。RTL 复位仅有硬件上拉，
不是软件共享复位；PHY 专有初始化、自动链路检测及 100BASE-T1 对端角色仍需实板验证。
片上启动 Flash 沿用同系列板的 1 MiB 与分区，SPI2 外置 Flash 暂不启用。

旧参考板的显式构建方式（独立输出目录）：

```sh
west build -s apps/ethercat_slave -b dust-hpm5e3y -d build/ethercat_legacy -p always
```
