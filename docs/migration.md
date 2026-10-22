# 迁移记录（2026-10-22）

来源：NoneOfEver/wbr_control，清理后提交 afc2e2d；共享硬件文件使用迁移时
工作树内容，实际源文件 SHA256 见 migration_sources.json。没有把原仓库整体复制。

USB 从 src/usb_can_bridge 迁出，底层必要文件进入 drivers/ 和 include/。
旧的 PC/MPC 条件分支被移除，保留现有 raw Bulk 编译路径、调度优先级与 ABI。
没有迁入 chassis 控制算法。迁出前后清理版本 USB bin 曾匹配 SHA256：
0082e0e04bee8696384aeb37dfaf15c467f9191da56b5fbba5c5b8e2b92746c5。
上述一致性比较使用相同的本地 HPM 驱动基线。另用固定版本 HPM 加本仓库 UART 补丁
完成两个应用的独立 workspace 构建，未包含本地 CAN 实验追踪改动；USB ROM 因而减少
80 字节。固定依赖构建哈希见 build_verification.json，不与上述同基线哈希混用。

EtherCAT 按用户确认，仅迁出现有 HPM5E3Y FreeRun 计数器从站基础。现有
MPC slave core 与底盘 fallback 属于算法工程，不混作通用 EtherCAT CAN 网关。
SSC 实现与生成配置仅本地导入，没有重新许可或发布到本仓库。

原 Zephyr/HPM/CherryUSB/SDK 作为第三方依赖保留在 workspace，由 west.yml 固定版本。
HPM UART 功能补丁单独保存，未把用户工作树中的 CAN 追踪实验改动夹带进来。

迁移不自动刷写，原已运行固件不受影响。新仓库完成编译不表示已完成新硬件验证。

2026-10-22 分支拆分：main/usb 仅保留 USB；EtherCAT 基础转至 ethercat 分支。
此前双应用构建记录为拆分前的历史验证。

2026-10-22 后续：UART RX 修复已提交到 HNUST-Dust/hpm_support，
west.yml 固定到 a31c06940e8d11ee360b970c89523a7727b63fff；删除补丁与 CI/README 手工应用步骤。
旧版构建与补丁记录仅为历史记录。

2026-10-23：通用协议 v2 在 feature/generic-usb-adapter 独立开发，移除机器人消息、
参数生成、电机地址和周期槽位；UART 参数仍固定。v0.1/main 保留旧协议以供回退。
配套 dust-control 同名分支；详见 GENERIC_USB_MIGRATION.md。

2026-10-23 后续：按维护者要求引入 config/can_devices.yaml，构建时生成设备地址、
接收过滤器和默认 CAN 保护表；上位机不得清除或覆盖 CAN 基线。UART 参数仍固定，
电机协议解析和控制算法仍在 NUC。历史“完全原始全接收/主机上传保护”描述由此策略替代。
