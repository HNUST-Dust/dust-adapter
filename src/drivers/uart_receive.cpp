/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/uart_receive.hpp"

#if defined(CONFIG_UART_HPMICRO_RX_LINE_IDLE)
#include <drivers/uart_hpmicro.h>
#endif

namespace platform {

int StartUartReceive(const device* dev, uart_callback_t callback, void* user,
                     uint8_t* buffer, size_t size, int32_t timeout_us) {
    if (!dev || !device_is_ready(dev)) {
        return -ENODEV;
    }

#if defined(CONFIG_UART_HPMICRO_RX_LINE_IDLE)
    // 检测器实现及 UART 与定时器的连接关系归驱动和板级配置所有。
    int result = uart_hpm_rx_line_idle_prepare(dev);
    if (result) {
        return result;
    }
#else
    int result;
#endif

    result = uart_callback_set(dev, callback, user);
    if (result) {
        return result;
    }
    result = uart_rx_enable(dev, buffer, size, timeout_us);
    if (result) {
        return result;
    }

#if defined(CONFIG_UART_HPMICRO_RX_LINE_IDLE)
    result = uart_hpm_rx_line_idle_start(dev, timeout_us);
    if (result) {
        (void)uart_rx_disable(dev);
        return result;
    }
#endif
    return 0;
}

bool UartTransmitIdle(const device* dev) {
    // HPM 的 DMA 完成早于 FIFO 排空；后续写入会重置 FIFO。
#if defined(CONFIG_UART_HPMICRO)
    return uart_irq_tx_complete(dev);
#else
    // 通用异步驱动须在 UART_TX_DONE 后允许复用发送缓冲区。
    (void)dev;
    return true;
#endif
}

}  // namespace platform
