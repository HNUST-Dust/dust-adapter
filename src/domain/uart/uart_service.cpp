/* SPDX-License-Identifier: Apache-2.0 */

/* UART 业务服务：发送窗口/重传/接收队列/超时，硬件通过 drivers::UartPort。 */

#include "domain/uart/uart_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ecat {

UartService::UartService(UartPort& port) : port_(port) {}

void UartService::AttachFaultSink(FaultSink sink) {
    fault_sink_ = sink;
}

int UartService::Initialize() {
    const int result = port_.Init();
    if (result) {
        return result;
    }
    for (unsigned index = 0; index < kPortCount; ++index) {
        ports_[index].ready = port_.Ready(uint8_t(index));
    }
    return 0;
}

void UartService::ApplyLimits(Limits& limits) const {
    for (unsigned index = 0; index < kPortCount; ++index) {
        if (!ports_[index].ready) {
            continue;
        }
        limits.uart_rx_mask |= 1U << index;
        if (index != 4) {  // UART5（SBUS）只有 RX。
            limits.uart_tx_mask |= 1U << index;
        }
    }
}

void UartService::Configure(const Configuration& value) {
    for (unsigned index = 0; index < kPortCount; ++index) {
        rx_capacity_[index] = value.uart[index].rx_bytes;
    }
}

void UartService::AbortAll() {
    epoch_ = 0;
    port_.SetRxEpoch(0);
    for (unsigned index = 0; index < kPortCount; ++index) {
        auto& port = ports_[index];
        if (port.ready && port_.Stats(uint8_t(index)).busy) {
            if (port_.Abort(uint8_t(index))) {
                ++port.protection_errors;
            }
        }
    }
}

void UartService::Stop() {
    AbortAll();
    for (unsigned index = 0; index < kPortCount; ++index) {
        auto& port = ports_[index];
        port.transmit.Clear();
        port.pending.length = 0;
        port.offset = 0;
        port.discontinuity = false;
        UartRxBlock discarded;
        while (port_.TakeRx(uint8_t(index), discarded)) {
        }
    }
}

void UartService::EnterActive() {
    for (unsigned index = 0; index < kPortCount; ++index) {
        auto& port = ports_[index];
        port.pending.length = 0;
        port.pending.epoch = 0;
        port.offset = 0;
        port.discontinuity = false;
        UartRxBlock discarded;
        while (port_.TakeRx(uint8_t(index), discarded)) {
        }
    }
    if (++epoch_ == 0 || epoch_ > INT32_MAX) {
        epoch_ = 1;
    }
    port_.SetRxEpoch(epoch_);
}

void UartService::Service(Session& session, uint64_t now) {
    for (unsigned index = 0; index < kPortCount; ++index) {
        auto& port = ports_[index];
        if (!port.ready) {
            continue;
        }
        if (port_.Faulted(uint8_t(index))) {
            port.fault = true;
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Transport);
            }
            return;
        }
        const UartStats stats = port_.Stats(uint8_t(index));
        if (stats.busy || !port_.TransmitIdle(uint8_t(index))) {
            if (now - port.started >= kTimeoutUs) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::UartTimeout);
                }
                return;
            }
            continue;
        }
        const Write* write = port.transmit.Front();
        if (!write) {
            continue;
        }
        if (now - write->accepted >= kTimeoutUs) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::UartTimeout);
            }
            return;
        }
        port.started = now;
        const int result = port_.Transmit(uint8_t(index), write->data.data(),
                                          write->size, kTimeoutUs);
        if (result) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Transport);
            }
            return;
        }
        port.transmit.Pop();
    }
    (void)session;
}

void UartService::Capture(Session& session, uint64_t now) {
    for (unsigned index = 0; index < kPortCount; ++index) {
        auto& port = ports_[index];
        if (session.state() != BusinessState::Active) {
            continue;
        }
        const unsigned capacity = rx_capacity_[index];
        if (!capacity) {
            UartRxBlock discarded;
            while (port_.TakeRx(uint8_t(index), discarded)) {
            }
            continue;
        }
        for (unsigned blocks = 0; blocks < 4; ++blocks) {
            if (port.offset == port.pending.length) {
                port.offset = 0;
                port.pending.length = 0;
                if (!port_.TakeRx(uint8_t(index), port.pending)) {
                    break;
                }
                if (port.pending.epoch != epoch_) {
                    port.pending.length = 0;
                    continue;
                }
                port.discontinuity |= port.pending.discontinuity;
            }
            /* now 由调用方在每个周期传入；块年龄以同一时间基准判定。 */
            if (uint32_t(uint32_t(now) - port.pending.capture_us) >= kTimeoutUs) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::UartTimeout);
                }
                return;
            }
            const size_t length =
                std::min(unsigned(port.pending.length - port.offset), capacity);
            if (!session.QueueUart(index,
                                   {port.pending.data.data() + port.offset,
                                    length},
                                   port.pending.capture_us, now,
                                   port.discontinuity)) {
                break;
            }
            port.offset += length;
            port.discontinuity = false;
        }
    }
}

bool UartService::Dispatch(Session& session, const Actions& actions,
                             uint64_t now) {
    for (unsigned index = 0; index < kPortCount; ++index) {
        const auto& delivery = actions.uart[index];
        if (!delivery.valid) {
            continue;
        }
        if (index == 4 || !ports_[index].ready ||
            delivery.bytes.size > 512 || !delivery.bytes.data) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Wire);
            }
            return false;
        }
        Write value;
        value.accepted = now;
        value.size = delivery.bytes.size;
        std::memcpy(value.data.data(), delivery.bytes.data, value.size);
        if (!ports_[index].transmit.Push(value)) {
            continue;  // 不 ACK；主站重传，不能重复写入已接纳块。
        }
        if (!session.CommitUart(index, delivery.sequence)) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Sequence);
            }
            return false;
        }
    }
    return true;
}

bool UartService::AnyFault() const {
    for (const auto& port : ports_) {
        if (port.fault) {
            return true;
        }
    }
    return false;
}

int UartService::Status(unsigned index, unsigned page,
                          std::array<uint32_t, 3>& values) const {
    if (index >= kPortCount) {
        return -EINVAL;
    }
    const UartStats stats = port_.Stats(uint8_t(index));
    if (!page) {
        values = {stats.received, stats.drops, stats.completed};
        return 0;
    }
    if (page == 1) {
        values = {stats.errors, uint32_t(stats.busy ? 1 : 0),
                  uint32_t(ports_[index].transmit.size())};
        return 0;
    }
    return -ENOTSUP;
}

uint32_t UartService::ProtectionErrors() const {
    uint32_t total = 0;
    for (const auto& port : ports_) {
        total += port.protection_errors;
    }
    return total;
}

uint32_t UartService::QueuedBlocks(uint8_t index) const {
    return index < kPortCount ? uint32_t(ports_[index].transmit.size()) : 0;
}

}  // namespace ecat
