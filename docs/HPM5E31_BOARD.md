# HPM5E31 原理图板级迁移

默认构建 `./scripts/build.sh ethercat -p always` 选择 `dust-hpm5e31`。
在仓库根目录直接执行 `west build -b dust-hpm5e31 -p always` 也会加载同一
EtherCAT 应用及其 prj.conf，默认输出到 `build`；不会编译根目录 USB 应用。

板级定义位于工作区 hpm_support 的 `boards/hpmicro/dust-hpm5e31`，由 west.yml
固定版本。[引脚表与原理图待确认项](https://github.com/HNUST-Dust/hpm_support/blob/b12efc50328047b73ac0fce5704d602ecb88fad2/boards/hpmicro/dust-hpm5e31/README.md)。

## 本次落实

- 新建独立板型，不修改 dust-hpm5e3y 的接线。
- 按原理图接入四路 CAN、UART0～6、RTL8201F/TJA1101B 的 MII、共享 MDIO/MDC
  和 REFCK；TJA1101B 使用 PF28 复位、PF26 中断。
- 不再对新板执行旧 SIP PHY 的 PV/PW、PA28 CTR1 和模拟焊盘实验初始化。
  旧配置移入 `boards/dust-hpm5e3y.overlay`，仅旧板构建加载。
- `prj.conf` 明确 `CONFIG_SPI=n`，SPI3 节点 disabled，暂不采集板载 IMU。
  CS PC21 默认高，IMU 加热门极 PD04 默认低，PWM 引脚仅启用蜂鸣器 PF01。
  外部 UART IMU 并未因此禁用。
- 原理图 WS2812 的 `PWM0_P2` 未连到 MCU，暂不启用，等待硬件接线确认。

## 验证及边界

新板和旧参考板的 EtherCAT SSC 应用均完成干净构建，未刷写硬件。
新板 ROM 94016 B、主 RAM 78408 B；旧参考板 ROM 96456 B、主 RAM 78408 B。
构建配置已核对为 HPM5E31、dust-hpm5e31、SPI 关闭，新板无 SPI 驱动目标。
上述为板级初次迁移的历史构建记录。当前应用已切换到 DTC1 实验对象字典与周期协议，
最新内存占用、ESI 和未完成项见 [SSC 网关接入](ETHERCAT_SSC_GATEWAY.md)。

PHY 地址先按电阻配置使用 RTL8201F=1、TJA1101B=0。RTL 复位仅有硬件上拉，
不是软件共享复位；PHY 专有初始化、自动链路检测及 100BASE-T1 对端角色仍需实板验证。
片上启动 Flash 沿用同系列板的 1 MiB 与分区，SPI2 外置 Flash 暂不启用。

旧参考板的构建结果属于旧驱动基线；新 HPM_ESC API 迁移仅验证 dust-hpm5e31。

## JTAG 连接

U1 使用自定义针序：3=TDI、4=TDO、5=nTRST、6=TMS、7=TCK、8=GND、9=3.3V；
10 为 5V，不能作为 3.3V 参考。nTRST 与 MCU RESET 不是同一信号。
OpenOCD 脚本已固定从当前 West 工作区 SDK 获取，初次联调默认 500 kHz。
路径修正后需要 `west build -b dust-hpm5e31 -p always` 清除旧缓存再生成配置。

TAP 识别失败发生在写入 Flash 之前。若同时出现 CMSIS-DAP 0x12 未实现和
0x14 响应错位，先核对实际接线及探针固件/后端通信，不能仅按全 1 的 ID 判断芯片故障。
配置和编译验证不代表 JTAG 已连接成功，尚未自动连接探针、复位或刷写。

## HPM_ESC 驱动接口迁移（2026-10-27）

同步后的 hpm_support 将 `ETHERCAT_HPMICRO` 替换为 `HPM_ESC`，公开头文件为
`zephyr/drivers/ethercat/hpm_esc.h`。本工程已迁移配置、读写、时钟、IRQ 回调和启动流程，
旧配置不能只改 Kconfig 名称后继续使用旧 C API。

SII 仍为本应用的 2048 字节 RAM 镜像，EEPROM 请求应答移至应用层，写入不持久化；
没有引入远端示例的 Flash EEPROM 格式化。计时使用 Zephyr 的毫秒时钟。
PDI 中断可由 SSC 单独屏蔽，FreeRun 仅使用 PDI/RESET，中断启动后关闭 SYNC0/1。

通过锁定的 hpm_support 版本直接复用 `samples/ecat/Middleware/ESC/hpm5e31_phy.c`
及其板级配置，以保留 RTL8201F/TJA1101B 初始化和 100 ms 软件链路轮询。
未引入该示例的 SSC、对象字典、EEPROM 镜像或 PDO；网关 SII/ESI 由本项目独立生成。
TJA1101B 扩展寄存器与实板链路仍需验证，构建通过不替代硬件验收。
SPI IMU 与加热仍关闭。
