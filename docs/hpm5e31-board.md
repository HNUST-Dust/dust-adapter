> 状态：living · 更新：2026-11-02 · 分支：ethercat

# HPM5E31 板级说明

默认构建 `./scripts/build.sh ethercat -p always` 选择板卡 `dust-hpm5e31`：

```sh
cd dust-adapter
./scripts/build.sh ethercat -p always
# 等价：west build -b dust-hpm5e31 -p always
```

板级定义位于 West 工作区 `hpm_support` 的 `boards/hpmicro/dust-hpm5e31`，
由 `west.yml` 固定版本；本仓库 `boards/dust-hpm5e31.overlay` 只补充应用层
DTS 别名和覆盖。引脚表与原理图确认项见
[板级 README](https://github.com/HNUST-Dust/hpm_support/blob/b12efc50328047b73ac0fce5704d602ecb88fad2/boards/hpmicro/dust-hpm5e31/README.md)。

## 接线与资源

- 四路 CAN（`mcan0..3`）经 DTS aliases `dust_can_0..3` 绑定；波特率、白名单和
  保护帧由 `config/can_devices.yaml` 编译期决定，见 [CAN 配置](../config/README.md)。
- UART0～6 经 aliases `dust_uart_1..6` 绑定；UART2 按 HI91 配置为 921600，
  UART5 为 SBUS 100000/8E2，其余沿用 DTS 波特率。
- EtherCAT 使用 HPM5E31 片上 ESC，外接 RTL8201F（Port0，PHY 地址 1）与
  TJA1101B（Port1，PHY 地址 0），共享 MDIO/MDC 与 REFCK；
  TJA1101B 使用 PF28 复位、PF26 中断。
- 板载 HXY IMU 使用 SPI3，CS=PC21、DRDY=PC18，DMA 使用 XDMA 6/7；
  详见 [板载 IMU](onboard-imu.md)。
- 蜂鸣器使用 PWM0 通道 1（PF01）；IMU 加热 MOSFET 使用 PWM0 通道 4，
  门极 PD04 默认低。两者周期由 PWM 计数器独立配置，寄存器更新由共享互斥锁串行化。
- 原理图 WS2812 的 `PWM0_P2` 未连到 MCU，暂不启用。

## 默认外设状态

`prj.conf` 当前启用 SPI3/XDMA 板载 IMU 原始采样、蜂鸣器与 IMU 加热 PID。
具体开关见 `Kconfig` 与 `prj.conf`。

## 驱动接口

`hpm_support` 同步后使用 `HPM_ESC` 驱动，公开头文件为
`zephyr/drivers/ethercat/hpm_esc.h`；本工程已迁移配置、读写、时钟、IRQ 回调和
启动流程，旧配置不能只改 Kconfig 名称后继续使用旧 C API。

SII 是本应用的 2048 字节 RAM 镜像，EEPROM 请求应答在应用层完成，写入不持久化；
没有引入 Flash EEPROM 格式化。计时使用 Zephyr 毫秒时钟。
PDI 中断可由 SSC 单独屏蔽，FreeRun 仅使用 PDI/RESET，启动后关闭 SYNC0/1 中断。

PHY 初始化复用锁定版本的 `hpm5e31_phy.c` 及其板级配置，保留
RTL8201F/TJA1101B 初始化和 100 ms 软件链路轮询；未引入该示例的 SSC、
对象字典、EEPROM 镜像或 PDO。网关 SII/ESI 由本项目独立生成，
见 [SSC 网关接入](ethercat-ssc-gateway.md)。

## JTAG 连接

U1 使用自定义针序：3=TDI、4=TDO、5=nTRST、6=TMS、7=TCK、8=GND、9=3.3V；
10 为 5V，不能作为 3.3V 参考。nTRST 与 MCU RESET 不是同一信号。
OpenOCD 脚本从当前 West 工作区 SDK 获取，初次联调默认 500 kHz。

TAP 识别失败发生在写入 Flash 之前。若同时出现 CMSIS-DAP 0x12 未实现和
0x14 响应错位，先核对实际接线及探针固件/后端通信，不能仅按全 1 的 ID 判断芯片故障。
配置和编译验证不代表 JTAG 已连接成功；仓库不自动连接探针、复位或刷写。

## 验证边界

构建通过不等于硬件验收。PHY 专有初始化、自动链路检测、100BASE-T1 对端角色、
TJA1101B 扩展寄存器、IMU 波形与加热效果仍需实板测量确认。
