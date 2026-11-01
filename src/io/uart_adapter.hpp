/* SPDX-License-Identifier: Apache-2.0 */

/* UART 适配层对外接口：由 io_backend facade 调用，状态由 uart_adapter.cpp 私有。 */

#ifndef ECAT_UART_ADAPTER_HPP_
#define ECAT_UART_ADAPTER_HPP_

#include <array>
#include <cstdint>

#include "protocol/protocol.hpp"

namespace ecat {

/* 首次初始化前建立接收队列；InitializeIo 在最早的失败路径之前调用。 */
void UartInitQueues();
int UartInitialize();
void UartStop();
void UartApplyLimits(Limits& limits);
void UartConfigure(const Configuration& value);
bool UartAnyFault();
int UartStatus(unsigned index, unsigned page, std::array<uint32_t, 3>& values);
void UartService(Core& core, uint64_t now);
void UartCapture(Core& core, Core::Feedback& feedback, uint64_t now);
bool UartDispatch(Core& core, const Actions& actions, uint64_t now);
/* 进入 Active：清空接收队列并推进 capture epoch。 */
void UartEnterActive();
uint32_t UartProtectionErrors();

}  // namespace ecat

#endif /* ECAT_UART_ADAPTER_HPP_ */
