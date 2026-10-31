# EtherCAT 从站

默认板卡 dust-hpm5e31，片上 ESC、外接 RTL8201F/TJA1101B，使用 SSC 5.13。
[板级接线与 IMU 禁用说明](HPM5E31_BOARD.md)。旧 dust-hpm5e3y 仅保留为历史参考。

当前默认应用已经从计数器切换到 **DTC1 实验网关**：SSC 动态对象字典、0x2100 配置、
固定 SM 分区、FreeRun 周期交换均已接入。CAN/UART 后端尚未接入，Arm 被拒绝，
不提供本地保护能力 bit31。不能用生产主机启动控制业务。
完整边界与测试见 [SSC 网关接入](ETHERCAT_SSC_GATEWAY.md)。

实验身份 VID=0、PID=0x44544331、Revision=0x00010000；新 ESI 为
`config/esi/DUST_HPM5E31_Gateway.xml`。旧 `DUST_HPM5E_FreeRun.xml`
仅归档旧计数器身份 0x57425231，不再描述当前构建。

## SSC 配置与许可

`external/ssc` 保留已获授权纳入的 SSC 5.13 文件及 Beckhoff 原始许可，不改为 Apache-2.0。
构建把 SSC 源码复制到输出目录 `ssc_gateway`，以自有 `src/ssc/sampleappl.h`、应用回调和
生成的 `eeprom.h` 替换计数器应用，原目录不改写，也不编译其 `sampleappl.c`。
SSC 约束通过生成的头文件覆盖 `ecat_def.h` 中的默认值。

显式外部目录仍受支持，必须提供与现有 hpm5e_ssc 硬件绑定兼容的 licensed SSC：

```sh
./dust-adapter/scripts/build.sh ethercat -- -DDUST_ADAPTER_SSC_DIR=/path/to/configured/ssc/src
```

配置、影像与保护规则见 [协议核心](ETHERCAT_PROTOCOL_CORE.md)。主机控制算法留在主机。
