# EtherCAT 从站基础迁移

板卡 dust-hpm5e3y，非 HPM6750。使用片上 ESC 与 SSC 5.13。
这是原 `ethercat_slave_test` 基础的独立化，不是新增 EtherCAT-to-CAN 功能。

当前 ABI：实验 Vendor ID=0、Product=0x57425231、Revision=0x00010000；
SM2 为 32-bit OutputCounter，SM3 为 32-bit InputCounter；CoE mailbox 各128B；
FreeRun，PDI AL-event IRQ，未启用 DC/SYNC0。SII 2048B RAM image 来自配置的 SSC。
ESI 在 `apps/ethercat_slave/esi/DUST_HPM5E_FreeRun.xml`。不要将其用于旧72/96B MPC ABI。

应用及硬件绑定位于 apps/ethercat_slave/src，原有 ESC/PHY 初始化、从站循环和
联调输出保留。本次不把底盘 MPC ChassisPort/force fallback 接口搬入适配器。

## SSC 外部依赖

Beckhoff SSC 实现及配置生成文件有独立许可，不随本仓库的 Apache 源码发布。
需自行提供原来可构建的 SSC 5.13 生成目录（通常名 ssc_generated/src）。
其中 ecat_def.h 必须引用 hpm5e_ssc 硬件绑定，sampleappl 和 eeprom 必须匹配此 ESI。
不要直接拿未经配置的 SSC 通用样例替代。tools/import_ssc.py 只复制本地文件，
保留许可头与哈希，不联网下载，也不覆盖已存在目录。

缺失 SSC 时 CMake 明确失败。显式外部目录参数可避免导入复制：

```bash
./dust-adapter/scripts/build.sh ethercat -- -DDUST_ADAPTER_SSC_DIR=/path/to/configured/ssc/src
```

原来工程里的 SSC 来源路径仅是本次迁移时的本机输入，不是新工程编译依赖。
未来需要 CAN/UART/IMU 转发时，应设计对象字典/ESI/PDO、时间戳/流连续性、
命令 lease 和 SAFEOP/INIT 失联行为，并与主站对齐后另行实现与验证。
