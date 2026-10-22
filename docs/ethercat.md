# EtherCAT 从站基础迁移

板卡 dust-hpm5e3y，非 HPM6750。使用片上 ESC 与 SSC 5.13。
这是原 `ethercat_slave_test` 基础的独立化，不是新增 EtherCAT-to-CAN 功能。

当前 ABI：实验 Vendor ID=0、Product=0x57425231、Revision=0x00010000；
SM2 为 32-bit OutputCounter，SM3 为 32-bit InputCounter；CoE mailbox 各128B；
FreeRun，PDI AL-event IRQ，未启用 DC/SYNC0。SII 2048B RAM image 来自配置的 SSC。
ESI 在 `apps/ethercat_slave/esi/DUST_HPM5E_FreeRun.xml`。不要将其用于旧72/96B MPC ABI。

应用及硬件绑定位于 apps/ethercat_slave/src，原有 ESC/PHY 初始化、从站循环和
联调输出保留。本次不把底盘 MPC ChassisPort/force fallback 接口搬入适配器。

## SSC 配置与许可

`external/ssc` 已包含匹配当前 ESI 的 SSC 5.13 配置源码，直接参与本地和 CI 构建。
按维护者确认的芯片厂商授权范围纳入，保留独立 Beckhoff 许可与原始文件头，
不改为 Apache-2.0。`ecat_def.h` 引用 hpm5e_ssc 硬件绑定，sampleappl/eeprom 匹配本 ESI。

显式外部目录仍受支持：

```bash
./dust-adapter/scripts/build.sh ethercat -- -DDUST_ADAPTER_SSC_DIR=/path/to/configured/ssc/src
```

未来需要 CAN/UART/IMU 转发时，应设计对象字典/ESI/PDO、时间戳/流连续性、
命令 lease 和 SAFEOP/INIT 失联行为，并与主站对齐后另行实现与验证。
