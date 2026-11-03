/* SPDX-License-Identifier: Apache-2.0 */

/* UART 业务服务：4 块发送窗口、ACK/重传、接收队列、缺口与超时。
 * 硬件动作通过 drivers::UartPort 完成。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/queue.hpp"
#include "common/types.hpp"
#include "common/abi.hpp"
#include "domain/protocol/session.hpp"
#include "drivers/uart/uart_port.hpp"

namespace ecat {

class UartService {
   public:
    explicit UartService(UartPort& port);
    void AttachFaultSink(FaultSink sink);

    int Initialize();
    void ApplyLimits(Limits& limits) const;
    void Configure(const Configuration& value);

    /* 进入 Active：清接收队列并推进 capture epoch。 */
    void EnterActive();
    /* 停止：清队列、推进 epoch、必要时 abort。 */
    void Stop();
    void AbortAll();

    /* 高频检查：发送推进、超时与故障。 */
    void Service(Session& session, uint64_t now);
    /* 常规发布前：把接收块交给 Session 的窗口。 */
    void Capture(Session& session, uint64_t now);

    bool Dispatch(Session& session, const Actions& actions, uint64_t now);
    bool AnyFault() const;
    int Status(unsigned index, unsigned page,
               std::array<uint32_t, 3>& values) const;
    uint32_t ProtectionErrors() const;
    uint32_t QueuedBlocks(uint8_t port) const;

   private:
    static constexpr unsigned kPortCount = 6;

    struct Write {
        uint64_t accepted = 0;
        uint16_t size = 0;
        std::array<uint8_t, 512> data{};
    };

    struct Port {
        IoQueue<Write, 4> transmit;
        UartRxBlock pending{};
        uint16_t offset = 0;
        bool ready = false;
        bool discontinuity = false;
        bool fault = false;
        uint32_t protection_errors = 0;
        uint64_t started = 0;
    };

    UartPort& port_;
    FaultSink fault_sink_{};
    std::array<Port, kPortCount> ports_{};
    std::array<uint16_t, kPortCount> rx_capacity_{};
    uint32_t epoch_ = 0;
};

}  // namespace ecat
