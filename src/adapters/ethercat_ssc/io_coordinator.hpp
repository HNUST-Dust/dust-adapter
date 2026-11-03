/* SPDX-License-Identifier: Apache-2.0 */

/* IO 协调器：把 CAN/UART/反馈服务组合成会话需要的一组操作。
 * 只做编排，不拥有硬件细节；保护编排在 ProtectionService。 */
#pragma once

#include <array>
#include <cstdint>

#include "domain/can/can_service.hpp"
#include "adapters/ethercat_ssc/feedback.hpp"
#include "domain/protocol/session.hpp"
#include "domain/uart/uart_service.hpp"

namespace ecat {

class IoCoordinator {
   public:
    IoCoordinator(CanService& can, UartService& uart,
                  FeedbackAggregator& feedback);
    void AttachFaultSink(FaultSink sink);

    int Initialize();
    bool Initialized() const {
        return initialized_;
    }
    uint32_t ProtectionErrors() const;

    bool CanCapacityValid(const Configuration& value) const;
    void ApplyLimits(Limits& limits) const;
    void Configure(const Configuration& value);

    void EnterActive(const Session& session, uint64_t now);
    void Stop();

    /* 每周期服务：CAN 提交、租约、UART 发送推进。 */
    void Service(Session& session, uint64_t now);
    /* 发布前采集：CAN 反馈槽 + UART 窗口 + IMU 最新样本。 */
    void Capture(Session& session, Feedback& feedback, uint64_t now);
    void CaptureCan(Feedback& feedback);
    bool Dispatch(Session& session, const Actions& actions, uint64_t now);
    bool DrainCan(uint64_t now);
    int QueryCanStatus(unsigned channel, unsigned page,
                       std::array<uint32_t, 3>& values) const;
    int QueryUartStatus(unsigned index, unsigned page,
                        std::array<uint32_t, 3>& values) const;

   private:
    CanService& can_;
    UartService& uart_;
    FeedbackAggregator& feedback_;
    FaultSink fault_sink_{};
    bool initialized_ = false;
};

}  // namespace ecat
