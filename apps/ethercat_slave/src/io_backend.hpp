/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ethercat/protocol.hpp"
namespace dust::ecat {
// 仅由 FreeRun 周期线程调用。初始化含启动保护及 NVS 启动计数提交，不在 ISR
// 调用。
int InitializeIo(uint64_t& boot);
Protection IoProtection();
void ApplyIoLimits(Limits& limits);
void ConfigureIo(const Configuration& configuration);
// 同步复制完整动作；失败会使 Core 进入保护并丢弃软件队列。
void DispatchIo(Core& core, const Actions& actions, uint64_t now);
// 高频 CAN 排空/快照更新不轮转 UART，返回快照是否变化。
bool ServiceCanReceive();
void CaptureCanFeedback(Core::Feedback& feedback);
// service 高频检查截止时间/故障；capture 在常规发布时采样 UART 窗口。
void ServiceIo(Core& core, uint64_t now);
void CaptureIo(Core& core, Core::Feedback& feedback, uint64_t now);
}  // namespace dust::ecat
