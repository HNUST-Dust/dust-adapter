/* SPDX-License-Identifier: Apache-2.0 */
/** @file gateway_runtime.hpp
 * @brief Firmware runtime entry and ownership contract.
 */
#ifndef DUST_ADAPTER_GATEWAY_RUNTIME_HPP_
#define DUST_ADAPTER_GATEWAY_RUNTIME_HPP_
namespace gateway {
/**
 * @brief Initialize hardware and run the single-owner session request loop.
 * @details Owns lease and protection policy. Callbacks communicate through
 * bounded queues and must not mutate policy. Boot protection precedes USB.
 * @return -1 on initialization failure; otherwise does not return.
 */
int RunGateway();
}  // namespace gateway
#endif  // DUST_ADAPTER_GATEWAY_RUNTIME_HPP_
