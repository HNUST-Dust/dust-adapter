# USB v4 协议代码阅读入口

`src/usb_protocol.hpp` 是协议的逻辑结构定义，按具名字段审阅消息。
`src/usb_protocol_codec.hpp` 是线上编码实现；新的消息接口在这里集中处理字节偏移、小端转换、
保留区清零和长度检查。`src/usb_session_policy.hpp` 是运行策略，不是报文结构。

逻辑结构不可通过 reinterpret_cast 映射 USB 缓冲区，不发送 sizeof(struct)。
结构体中的 padding 不属于协议；固定魔数、版本、消息类型及保留区由编码器补入。
结构化 Decode 接收已收集完整的 128 字节下行缓冲；原始实际长度须先由传输调用者
验证。Encode 失败时不得发送输出缓冲区，Decode 失败时不得执行业务动作。

| 功能 | 下行结构 | 上行结构 |
|---|---|---|
| 建立会话 | HelloRequest | ReadyReport |
| CAN | CanTransmitRequest | CanReceiveReport / CanReceivedFrame |
| UART | UartTransmitRequest | UartReceiveReport |
| IMU | 无 | ImuSampleReport |
| 保护配置 | ConfigureSafetyRequest | RequestResultReport |
| 许可与停止 | ArmRequest / StopRequest / HeartbeatRequest | 按运行时规则回复 |
| 查询状态 | StatusRequest | SessionStatusReport / CanStatusReport / UartStatusReport |

请求头明确命名 request_sequence，上报头明确命名 transmit_timestamp_us。
CAN 接收记录另有 receive_timestamp_us 与 bus_sequence，不与发送时间混淆。
UART 停止动作使用 UartStopAction，与 CanFrame 在逻辑上分开。

旧类型别名和 `protocol.hpp` 转接入口已移除。字节访问工具与 View/Builder
属于编码实现，新业务代码使用具名消息结构。请求构造直接使用具名消息结构及 Encode()，不再保留 client.hpp。

本轮是源码结构正式整理，不改变 wire v4，不代表固件已经硬件验收、合并 main
或发布。上位机现有 v4 线上编码可继续通信，源码类型共享仍需后续统一维护。
