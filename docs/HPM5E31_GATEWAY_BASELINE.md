# HPM5E31 网关开发起点

日期：2026-10-26。基线合并：f70e6f5。

新板适配已随后落实，当前默认 dust-hpm5e31，见 [板级迁移说明](HPM5E31_BOARD.md)。
下文“本次验证”记录原基线，不能当作新板的硬件验证。
feature/generic-usb-adapter 75acdfb 已合入 ethercat，未改动 USB v4 wire 与保护行为，
未更改外部 hpm_support/SDK/SSC 实现，不自动刷写。

## 当前工程

- 根目录 USB 应用保留通用 CAN、UART、IMU、温控、指示与保护配置，作为复用基础。
- apps/ethercat_slave 保留旧 ESC/PHY 与 SSC FreeRun 计数器；external/ssc 保留原许可。
- 默认板为按新原理图定义的 dust-hpm5e31；旧 dust-hpm5e3y 保留作参考。
- scripts/build.sh 默认 ethercat，显式 usb 可验证原基线；两个输出目录分离。

## 主机对齐依据

主机 dust-control 的 dev/wbr_infantry_ethercat 当前版本为 cea3cfe：
[主从契约](https://github.com/HNUST-Dust/dust-control/blob/cea3cfe/docs/ETHERCAT_RUNTIME.md)、
[布局生成器](https://github.com/HNUST-Dust/dust-control/blob/cea3cfe/src/hardware/ethercat/pdo_config.cpp)。
旧计数器 ESI、实验 Vendor ID=0 与旧 MPC ABI 均不能作为新网关协议使用。

已确认方向：控制与设备协议解析留在主机；1 ms 通信；主机 YAML 配置 1～3 块板的
通道/容量；保留全部原 USB 能力；CAN 反馈不可靠重传并优先最新；UART 独立窗口。
普通主机 PDO 不得删除编译期 CAN 白名单或本地保护表。

## 后续实现顺序

1. 实现可独立测试的配置/PDO 字段与状态机，对齐主机显式小端编码、布局签名、
   0x2100 容量/周期/保护配置、会话/启动标识、来源独立期限及停止语义。
2. 扩展 SSC 对象字典、动态映射、SM 资源校验和 ESI；不只改 XML 就宣称固件支持。
   先核实 ESC RAM/三缓冲与邮箱布局；现有 main.c 的 0x1800 RAM 写探针须在新映射前移除或关闭。
3. 从 USB 应用抽出实际可复用的 CAN/UART/原始 IMU 交付，替换 USB 报告发布依赖；
   保持设备数据不在 MCU 解析，继承本地保护和温控兜底。
4. 新板引脚已按原理图定义，旧 SIP PHY overlay 已隔离到旧板。后续实板核验
   外置 PHY 地址/链路、CAN/UART 物理收发；逻辑编号与物理外设映射继续分离。
5. 跨主从编解码向量、启动/SAFEOP/INIT/断线/复位/旧目标/队满故障测试；
   先无输出单板，再三板，再由现场操作人员执行使能验收。

## 本次验证

- generic 协议/CAN 白名单与保护/温控/USB v4 消息合约测试通过（ASan/UBSan）。
- 两种 build.sh 目标与默认路由检查通过；脚本语法与 git diff 检查通过。
- 通过 OrbStack 宿主桥接调用现有 macOS Zephyr SDK 0.16.5/West/Python 环境，
  不在 Linux 执行 Mach-O 工具链，不自动更新依赖仓库。
- 已完成旧 EtherCAT 从站构建：ROM 96456 B，主 RAM 78408 B；
  输出 build/ethercat_slave/zephyr/zephyr.elf 与 zephyr.bin。
- USB generic 基线也完成独立构建：ROM 93596 B，主 RAM 124368 B；输出 build/usb_gateway/zephyr。
- 现有 SSC 源码构建存在旧警告，未在本次合并中改写第三方实现。
- 此阶段没有实现新网关 PDO、没有刷写或运行硬件，编译结果不替代验收。
