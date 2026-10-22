# 迁移记录（2026-10-22）

来源：NoneOfEver/wbr_control，清理后提交 afc2e2d；共享硬件文件使用迁移时
工作树内容，实际源文件 SHA256 见 migration_sources.json。没有把原仓库整体复制。

USB 从 src/usb_can_bridge 迁出，底层必要文件进入 drivers/ 和 include/。
旧的 PC/MPC 条件分支被移除，保留现有 raw Bulk 编译路径、调度优先级与 ABI。
没有迁入 chassis 控制算法。迁出前后清理版本 USB bin 曾匹配 SHA256：
0082e0e04bee8696384aeb37dfaf15c467f9191da56b5fbba5c5b8e2b92746c5。
后续纯符号/目录整理后会重新记录最终构建哈希。

EtherCAT 按用户确认，仅迁出现有 HPM5E3Y FreeRun 计数器从站基础。现有
MPC slave core 与底盘 fallback 属于算法工程，不混作通用 EtherCAT CAN 网关。
SSC 实现与生成配置仅本地导入，没有重新许可或发布到本仓库。

原 Zephyr/HPM/CherryUSB/SDK 作为第三方依赖保留在 workspace，由 west.yml 固定版本。
HPM UART 功能补丁单独保存，未把用户工作树中的 CAN 追踪实验改动夹带进来。

迁移不自动刷写，原已运行固件不受影响。新仓库完成编译不表示已完成新硬件验证。
