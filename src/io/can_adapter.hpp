/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 适配层对外接口：由 io_backend facade 调用，状态由 can_adapter.cpp 私有。 */

#ifndef ECAT_CAN_ADAPTER_HPP_
#define ECAT_CAN_ADAPTER_HPP_

#include <array>
#include <cstdint>

#include "protocol/protocol.hpp"

namespace ecat {

/* 首次初始化前建立接收队列；InitializeIo 在最早的失败路径之前调用。 */
void CanInitQueues();
int CanInitialize();
void CanStop();
void CanPump(Core* core, uint64_t now);
bool CanValidate(void* user, unsigned index, const CanFrame& frame);
void CanApplyLimits(Limits& limits);
bool CanCapacityValid(const Configuration& value);
void CanConfigure(const Configuration& value);
bool CanDrainReceive();
void CanCaptureFeedback(Core::Feedback& feedback);
bool CanAnyFault();
int CanStatus(unsigned channel, unsigned page, std::array<uint32_t, 3>& values);
bool CanDispatch(Core& core, const Actions& actions, uint64_t now);
/* 进入 Active：设置 CAN 命令 lease 截止时间。 */
void CanEnterActive(Core& core, uint64_t now);
/* 当前是否存在已过期的命令 lease。 */
bool CanLeaseExpired(uint64_t now);
uint32_t CanProtectionErrors();

}  // namespace ecat

#endif /* ECAT_CAN_ADAPTER_HPP_ */
