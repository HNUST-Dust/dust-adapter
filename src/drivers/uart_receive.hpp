/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/drivers/uart.h>

namespace platform {

/**
 * @brief 安装回调并启动异步 UART 接收，按配置启用板级空闲检测。
 * @details 在线程上下文、接收尚未启动时调用。缓冲区须满足驱动的 DMA
 * 要求，且在驱动释放前保持有效；后续缓冲区由调用者通过回调提供。
 * @return 成功返回零，否则返回负 errno。检测器启动失败时请求停止接收。
 */
int StartUartReceive(const device* dev, uart_callback_t callback, void* user,
                     uint8_t* buffer, size_t size, int32_t timeout_us);

/** @brief 返回硬件发送器是否已排空，可安全开始下一次异步发送。 */
bool UartTransmitIdle(const device* dev);

}  // namespace platform
