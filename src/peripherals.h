/* SPDX-License-Identifier: Apache-2.0 */
/** @file peripherals.h
 * @brief Peripheral publication and output admission interfaces.
 */
#pragma once
#include "protocol.hpp"
namespace gateway {
/**
 * @brief Return the current session identifier; zero means inactive.
 */
uint32_t Session();
/**
 * @brief Enqueue a session-tagged peripheral packet without waiting; return
 * false on rejection.
 */
bool PublishPeripheral(const dust::wire::Packet&);
// Capability bits: CAN=1, UART10=2, UART2=4, onboard raw IMU=8, UART3=16.
/**
 * @brief Start fixed UART and IMU producers; return capability bits CAN=1,
 * UART10=2, UART2=4, IMU=8, UART3=16.
 */
uint32_t StartPeripherals();
/**
 * @brief Atomically consume the latched UART output fault.
 */
bool TakePeripheralFault();
/**
 * @brief Return whether the requested logical UART channel is initialized.
 */
bool UartAvailable(uint8_t channel);
/**
 * @brief Copy a UART payload into its bounded output queue.
 * @return Zero on admission, or negative errno for invalid, unavailable or full
 * output.
 */
int SubmitUart(uint8_t channel, const uint8_t* data, size_t length);
/**
 * @brief Discard software-queued UART writes; already active hardware transfers
 * cannot be recalled.
 */
void DiscardUartWrites();
/** @brief Snapshot of per-port receive and transmit counters. */
struct UartStats {
    uint32_t received, dropped, tx_completed, tx_errors;
};
/**
 * @brief Return a snapshot of per-channel counters; unknown channels yield zero
 * counters.
 */
UartStats ReadUartStats(uint8_t channel);
}  // namespace gateway
