> 状态：living · 更新：2026-11-02 · 分支：ethercat

# EtherCAT 从站

默认板卡 `dust-hpm5e31`：HPM5E31 片上 ESC、外置 RTL8201F/TJA1101B PHY，
SSC 5.13，**FreeRun** 周期交换，单 SSC 主线程。板级接线见
[HPM5E31 板级说明](hpm5e31-board.md)。

## 当前能力

- 动态 PDO：SSC 对象字典在构建期注册，`0x2100:1..22` 配置、Rx/Tx 映射与
  `0x1c12/0x1c13` 分配对象全部可用；主站按实际布局下载映射后进入 SAFEOP/OP。
- CAN 与 UART 后端已接入：MCAN 批次收发、UART 原生 idle/DMA 收发、
  MCU 侧白名单、保护帧、本地停机回调均已安装；初始化成功后声明 capability
  bit31 并允许 Arm。后端细节见 [SSC 网关接入](ethercat-ssc-gateway.md)。
- 启动身份：boot_id 由芯片 UID 派生值与预留 Flash 递增计数生成，先持久化并
  读回再对外暴露，跨启动去重。
- 板载 IMU、蜂鸣器与 IMU 加热 PID 已启用。
- 一次已进入 SAFEOP/OP 的会话结束后，主循环在安全上下文重建网关
  （新 boot_id、清空映射/判据/过程影像），主站下一次启动可直接重新配置
  `0x2100`，不需要断电或 ESC 复位。
- 可选 `CONFIG_DUST_ECAT_GATEWAY_DIAG` 在 PREOP/SAFEOP/OP 回调与发布被拒时打印
  原因，每次初始化限流，正常周期不刷屏。

## 边界

- 会话内不允许热改配置：Ready/Active/Protected 状态下再次写 `0x2100` 会被拒绝；
  rearm 只发生在会话结束后的安全上下文。
- FreeRun 不提供 SM/DC 时钟同步，也不把主从时钟视为已同步。
- 实验身份 VID=0x0d57、PID=0x44544331、Revision=0x00010000；VID 为实验厂商号，
  仅用于台架，不应作为生产身份使用。构建和离线测试通过不等于实板验收。
- 控制算法留在主机；从站不解释电机协议，只转发原始 CAN 字节并执行本地保护。

## SSC 配置与许可

`src/ecat/generated` 保留已获授权纳入的 SSC 5.13 文件及 Beckhoff 原始许可，
**不适用** 本仓库的 Apache-2.0，见 [SSC 许可](../src/ecat/generated/README.md)。

构建时 CMake 把 SSC 源码复制到构建目录 `ssc_gateway`，用自有
`src/ecat/sampleappl.h`、应用回调和生成的 `eeprom.h`/SII 替换计数器应用；
原目录不改写，也不编译其 `sampleappl.c`。SSC 约束通过生成的
`gateway_config.h` 覆盖 `ecat_def.h` 默认值，DC 相关配置在构建期被拒绝。

显式外部 SSC 目录仍受支持，必须提供与当前 `hpm_support` 硬件绑定兼容的
licensed SSC：

```sh
./scripts/build.sh ethercat -- -DDUST_ADAPTER_SSC_DIR=/path/to/configured/ssc/src
```

## 构建

在已初始化的 West 工作区中：

```sh
cd dust-adapter
./scripts/build.sh ethercat -p always
# 等价：west build -b dust-hpm5e31 -p always
```

可选台架 CAN profile 通过
`-DDUST_ADAPTER_CAN_CONFIG=/绝对路径/config/labs/can_bench_rx_only.yaml`
指定，默认使用 `config/can_devices.yaml`。

## 验证

```sh
./scripts/test_protocol.sh
```

覆盖协议/布局/PDO 字典/SSC/IO 后端/驱动边界的原生合约测试，以及 ESI/schema
一致性与 CAN profile 生成器检查。实板上的枚举、OP/Discover 与长时间稳定性
仍需独立验收。
