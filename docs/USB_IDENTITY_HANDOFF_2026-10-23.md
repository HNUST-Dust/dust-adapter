# dust-control 交接：MCU USB 设备身份变更

日期：2026-10-23

## 需要处理的问题

MCU 不再以固定 PID `0xFFFF` 枚举。PID 由每颗 MCU 的 UID 确定性派生，
dust-control 如果仍固定匹配旧 VID/PID，将无法发现更新后的固件。
本次不改变 USB v2 报文、端点和传输大小，只改变设备身份描述符。

## 固件侧最终规则

| 字段 | 当前规则 |
|---|---|
| VID | 保留 `USBD_VID`，当前为 `0x34B7`（HPMicro） |
| PID | MCU 完整 UID 的 CRC16-CCITT-FALSE |
| Manufacturer | `Dust` |
| Product | `CONFIG_BOARD`，当前为 `dust-hpm6750` |
| Serial Number | 完整 UID 的大写十六进制字符串；HPM6750 为 32 个字符 |

HPM6750 从 OTP shadow 的 `OTP_SOC_UUID_IDX` 起读取 `OTP_SOC_UUID_LEN`
字节（当前 16 字节），每个 32 位字按低字节到高字节拼接。不烧写 OTP。
PID 算法：多项式 `0x1021`、初值 `0xFFFF`、输入/输出均不反射、最终异或为零。
CRC 的全部 16 位直接作为 PID，不另外避开零或 `0xFFFF`。
USB 描述符中的 PID 按小端编码；libusb 返回的 `idProduct` 可直接作为数值使用。

同一芯片每次启动生成相同 PID；不同 UID 可能产生相同 PID，因此 PID
不能当作设备唯一标识。需要指定某块板时，应使用完整 Serial Number。
UID 全零或全 `0xFF` 时固件拒绝启动 USB，不回退到公共序列号。

VID 本轮没有修改，也没有取得新的 VID 分配。不要把厂商字符串 `Dust`
理解成独占 VID；VID/PID 和字符串均不构成认证。

## dust-control 需要修改的内容

1. 检查 `modules/usb/usb_link.cpp` 及设备配置，移除只接受 PID `0xFFFF`
   的默认发现逻辑；不要仅用固定 VID/PID 的单设备快捷打开接口。
2. 枚举设备，按 VID 筛选候选，再读取厂商、产品、序列号及接口描述符。
   产品名称应允许配置，以支持后续其他 board；当前目标是 `Dust` /
   `dust-hpm6750`。继续核对现有 vendor interface、Bulk 端点和 USB v2
   握手结果，不因 PID 变化而放宽协议校验。
3. 支持配置完整序列号来选择设备。多个匹配设备且未指定序列号时明确报错，
   不静默选择列表中的第一块板。权限不足或字符串读取失败时给出可定位错误。
4. 检查 Linux udev 规则、部署脚本和文档，避免仍限定旧 PID。若按 VID
   授权会扩大到该 VID 下的其他设备，应尽可能加入产品或接口等匹配条件，
   并验证规则效果。
5. 若保留显式 PID 配置，应明确其是精确筛选条件；默认发现流程不能继续
   隐含旧 PID。日志建议显示 VID、PID、产品名和完整序列号，方便选板。

实际端点地址及 USB v2 握手仍应以现有代码和固件描述符为准；本次无需为了
设备发现修改 wire 定义。旧固件支持策略由 dust-control 自行明确，不加入
绕过 v2 校验的宽松回退。

## 验证要求

- 用模拟设备列表验证：不同 PID、同 PID 不同序列号、指定序列号、多个候选、
  无候选及无权限路径。
- 固件刷入后核对实际枚举字段；重启后 PID 和序列号应保持不变。
- 至少两块板分别枚举，确认发现和按序列号选择正常；不能以两块板 PID 不同
  推导算法不会碰撞。
- 在实际 NUC 用户权限下验证 udev、打开接口及完整 Hello/Ready/Arm 流程。
  Arm 与实际电机输出仍按既有验收授权执行。

## 当前状态与代码位置

MCU 侧已实现并构建通过；尚未刷板、未验证实际 UID 读取或 USB 枚举。
dust-control 尚未因本次变更修改。交接时 MCU 改动仍在工作区，未为本次变更
单独提交，不应假定远程分支已包含这些内容。

MCU 主工作区：`/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/dust-adapter`。

- `src/drivers/usb_identity.cpp/.hpp`：UID 读取、序列号和 PID 生成。
- `src/drivers/usb_session.cpp`：枚举字符串及 USB 启动前填写动态 PID。
- `docs/migration.md`：USB 身份说明。

本文件仅交接 USB 身份变更。此前新增 UART11/12/15 等改动另需同步，不能因
完成设备发现修改就认为两仓库已全部对齐。
