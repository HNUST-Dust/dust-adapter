/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <dust_adapter/protocol.hpp>
namespace gateway {
uint32_t Session();
bool PublishPeripheral(const dust::wire::Packet &);
// Capability bits: CAN=1, UART10=2, UART2=4, onboard raw IMU=8, UART3=16.
uint32_t StartPeripherals();
bool TakePeripheralFault();
bool UartAvailable(uint8_t channel);
int SubmitUart(uint8_t channel, const uint8_t *data, size_t length);
void DiscardUartWrites();
struct UartStats {
  uint32_t received, dropped, tx_completed, tx_errors;
};
UartStats ReadUartStats(uint8_t channel);
} // namespace gateway
