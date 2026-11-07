/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 业务服务：白名单校验、批次与租约、保护帧、每 ID 最新反馈槽与统计。
 * 纯逻辑，硬件动作通过 drivers::CanBusPort 完成。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/queue.hpp"
#include "common/types.hpp"
#include "domain/can/can_topology.hpp"
#include "domain/can/latest_can.hpp"
#include "common/abi.hpp"
#include "domain/pdo/layout.hpp"
#include "domain/protocol/types.hpp"
#include "drivers/can/can_bus.hpp"

namespace ecat {

struct PendingCan {
    CanFrame frame;
    uint64_t deadline_us = 0;
};

inline bool CanExpired(const PendingCan& value, uint64_t now) {
    return value.deadline_us && now >= value.deadline_us;
}

class CanService {
   public:
    CanService(CanBusPort& bus, const CanTopology& topology);
    void AttachFaultSink(FaultSink sink);

    /* 校验编译期拓扑并初始化控制器。 */
    int Initialize();
    void ApplyLimits(Limits& limits) const;
    bool CapacityValid(const Configuration& value) const;
    void Configure(const Configuration& value);

    /* 会话进入 Active：按授权来源建立命令租约。 */
    void EnterActive(uint32_t active_sources, uint64_t now);
    bool LeaseExpired(uint64_t now) const;

    /* 会话结束/保护：清队列、撤销在途、必要时停控制器，并下发保护帧。 */
    void Stop();
    /* 仅清空与撤销，不发送保护帧（供 ProtectionService 编排顺序）。 */
    void ClearAndAbort();
    void EmitProtectionFrames();

    /* 周期服务：把队列提交给硬件、检查租约与故障。 */
    void Pump(uint64_t now);
    bool AnyFault() const;

    /* ISR 环 -> 每 ID 最新反馈槽。 */
    bool DrainReceive(uint64_t now);
    void CaptureFeedback(Feedback& feedback);

    bool Dispatch(const Actions& actions, uint64_t now);
    int Status(unsigned channel, unsigned page,
               std::array<uint32_t, 3>& values) const;
    uint32_t ProtectionErrors() const;
    uint32_t QueuedFrames(uint8_t bus) const;

    /* 供 ProtectionService 安装到 Session::Protection 的白名单校验。 */
    bool ValidateFrame(const CanFrame& frame) const;
    int CopyProtectionFrames(uint8_t bus, CanFrame* frames, size_t capacity) const;
    uint32_t ProtectionSignature() const {
        return topology_.protection_signature;
    }
    uint32_t CommandTimeoutUs() const {
        return topology_.command_timeout_us;
    }
    bool ProtectOnBoot() const {
        return topology_.protect_on_boot;
    }

   private:
    static constexpr unsigned kBusCount = 4;
    static constexpr unsigned kMaxBatch = 16;

    struct Port {
        IoQueue<PendingCan, 32> transmit;
        std::array<CanSample, 32> feedback{};
        LatestCanSlots latest;
        uint32_t batch = 0;
        uint32_t rx_sequence = 0;
        uint32_t drain_drops = 0;
        uint32_t submitted = 0;
        uint32_t errors_seen = 0;
        uint32_t protection_errors = 0;
        unsigned feedback_count = 0;
        uint64_t deadline = 0;
        bool ready = false;
        bool fault = false;
    };

    CanBusPort& bus_;
    const CanTopology& topology_;
    FaultSink fault_sink_{};
    std::array<Port, kBusCount> ports_{};
    std::array<uint64_t, 2> source_deadlines_{};
    std::array<uint8_t, 4> can_rx_enabled_{};
};

}  // namespace ecat
