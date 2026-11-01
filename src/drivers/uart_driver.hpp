/* SPDX-License-Identifier: Apache-2.0 */

/* UART 硬件驱动：端口配置、接收缓冲切换、原始块收发与中止。
 * 本层不包含 EtherCAT/协议语义。 */

#ifndef DRIVERS_UART_DRIVER_HPP_
#define DRIVERS_UART_DRIVER_HPP_

#include <cstddef>
#include <cstdint>

#include <zephyr/device.h>

namespace drivers {

enum class UartEventType {
    RxBlock,
    RxGap,
    RxFault,
    TxDone,
    TxAborted,
};

struct UartPortConfig {
    const device* dev = nullptr;
    uint8_t index = 0;
    bool configure_8e2 = false;  // 1 start bit + 8 data + even parity + 2 stop
};

/* 事件回调在中断上下文调用；data 仅对 RxBlock 有效。 */
using UartEventCallback = void (*)(void* context, uint8_t port,
                                   UartEventType type, const uint8_t* data,
                                   size_t length);

int UartInit(const UartPortConfig* ports, size_t count, UartEventCallback event,
             void* context);
bool UartReady(uint8_t port);
bool UartTransmitIdle(uint8_t port);
int UartTransmit(uint8_t port, const uint8_t* data, size_t length,
                 uint32_t timeout_us);
int UartAbort(uint8_t port);

}  // namespace drivers

#endif /* DRIVERS_UART_DRIVER_HPP_ */
