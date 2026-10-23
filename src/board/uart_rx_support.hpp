/* SPDX-License-Identifier: Apache-2.0 */
/** @file uart_rx_support.hpp
 * @brief Optional board support for UART partial-buffer publication.
 */
#pragma once
#include <zephyr/device.h>

#include <cstdint>
namespace board {
/** @brief Prepare board RX detection before enabling asynchronous reception.
 * @return Zero on success or negative errno. Ordinary UARTs need no setup.
 */
int PrepareUartReceive(const device* dev);
/** @brief Start optional idle detection after DMA reception is enabled.
 * @param dev Receiving UART device.
 * @param timeout_us Requested quiet interval in microseconds.
 * @return Zero on success or negative errno.
 */
int StartUartReceive(const device* dev, uint32_t timeout_us);
}  // namespace board
