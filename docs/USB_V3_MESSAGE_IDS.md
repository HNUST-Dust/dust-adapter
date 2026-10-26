# USB v3 消息编号交接（2026-10-24）

两仓库同时变更；v2 不兼容，必须配套更新，不做版本回退。
包头版本字节（偏移 8）改为 3，其余字段布局及包长保持不变。

| 编号 | 消息 |
|---|---|
| 1 | Hello |
| 2 | Ready |
| 3 | CanTransmitRequest |
| 4 | CanReceiveReport |
| 5 | UartTransmitRequest |
| 6 | UartReceiveReport |
| 7 | ImuSampleReport |
| 8 | ConfigureSafety |
| 9 | Arm |
| 10 | Stop |
| 11 | Heartbeat |
| 12 | Result |
| 13 | Status |

Transmit/Receive 始终以 MCU 为参照。旧类型名称及相关引用已同步调整。
版本校验拒绝 v2 包，防止旧编号在新端被解释为另一种消息。
MCU 定义位于 src/usb_protocol.hpp；主机位于 include/dust_adapter/protocol.hpp。
本次仅同步消息命名、编号及版本，不代表此前 UART 扩展等改动已全部同步。
