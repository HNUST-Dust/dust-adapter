/* SPDX-License-Identifier: Apache-2.0 */
/** @file uart_rx_support.cpp
 * @brief Default UART support for boards with native asynchronous RX timeout.
 */
#include "board/uart_rx_support.hpp"
namespace board {
int PrepareUartReceive(const device*) { return 0; }
int StartUartReceive(const device*, uint32_t) { return 0; }
}  // namespace board
