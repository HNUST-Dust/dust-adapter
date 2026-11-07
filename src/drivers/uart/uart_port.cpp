/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/uart/uart_port.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ecat {
namespace {
UartPort::Hi91Consumer hi91_consumer = nullptr;
}

void UartPort::SetHi91Consumer(Hi91Consumer consumer) {
    const auto key = irq_lock();
    hi91_consumer = consumer;
    irq_unlock(key);
}

UartPort::UartPort(const UartPortConfig* ports, size_t count) {
    port_count_ = std::min(count, kPortCount);
    atomic_set(&epoch_, 0);
    for (size_t i = 0; i < port_count_; ++i) {
        auto& port = ports_[i];
        port.dev = static_cast<const device*>(ports[i].device);
        port.index = ports[i].index;
        port.rx_only = ports[i].rx_only;
        atomic_clear(&port.busy);
        atomic_clear(&port.fault);
        atomic_clear(&port.received);
        atomic_clear(&port.drops);
        atomic_clear(&port.completed);
        atomic_clear(&port.errors);
        atomic_clear(&port.gap);
        atomic_clear(&port.cancelling);
        k_msgq_init(&port.rx, port.storage, sizeof(UartRxBlock), kRxDepth);
    }
}

void UartPort::EventTrampoline(void* context, uint8_t index,
                               drivers::UartEventType type, const uint8_t* data,
                               size_t length) {
    auto* self = static_cast<UartPort*>(context);
    if (index >= self->port_count_) {
        return;
    }
    auto& port = self->ports_[index];
    switch (type) {
        case drivers::UartEventType::TxDone:
            atomic_inc(&port.completed);
            atomic_clear(&port.cancelling);
            atomic_clear(&port.busy);
            break;
        case drivers::UartEventType::TxAborted:
            if (!atomic_cas(&port.cancelling, 1, 0)) {
                atomic_inc(&port.errors);
                atomic_set(&port.fault, 1);
            }
            atomic_clear(&port.busy);
            break;
        case drivers::UartEventType::RxGap:
            atomic_set(&port.gap, 1);
            atomic_inc(&port.drops);
            break;
        case drivers::UartEventType::RxFault:
            atomic_set(&port.fault, 1);
            break;
        case drivers::UartEventType::RxIdleBlock:
        case drivers::UartEventType::RxFullBlock:
        case drivers::UartEventType::RxBlock: {
            const uint32_t epoch = uint32_t(atomic_get(&self->epoch_));
            if (!epoch || !length) {
                break;
            }
            if (index == 1 && hi91_consumer && length <= 128) {
                hi91_consumer(epoch, data, length,
                    uint32_t(k_ticks_to_us_floor64(k_uptime_ticks())),
                    atomic_get(&port.gap) != 0);
                atomic_inc(&port.received);
                atomic_clear(&port.gap);
                break;
            }
            UartRxBlock block;
            block.port = index;
            block.epoch = epoch;
            block.capture_us =
                uint32_t(k_ticks_to_us_floor64(k_uptime_ticks()));
            if (length > block.data.size()) {
                atomic_inc(&port.drops);
                atomic_set(&port.gap, 1);
                break;
            }
            block.length = uint16_t(length);
            block.discontinuity = atomic_get(&port.gap) != 0;
            std::memcpy(block.data.data(), data, length);
            atomic_inc(&port.received);
            if (k_msgq_put(&port.rx, &block, K_NO_WAIT)) {
                atomic_inc(&port.drops);
                atomic_set(&port.gap, 1);
            } else {
                atomic_clear(&port.gap);
            }
            break;
        }
    }
}

int UartPort::Init() {
    drivers::UartPortConfig configs[kPortCount]{};
    for (size_t i = 0; i < port_count_; ++i) {
        configs[i] = {ports_[i].dev, ports_[i].index,
                      ports_[i].index == 4};
    }
    const int result = drivers::UartInit(configs, port_count_,
                                         EventTrampoline, this);
    if (result) {
        return result;
    }
    for (size_t i = 0; i < port_count_; ++i) {
        ports_[i].ready = drivers::UartReady(ports_[i].index);
    }
    return 0;
}

bool UartPort::Ready(uint8_t index) const {
    return index < port_count_ && ports_[index].ready;
}

bool UartPort::TransmitIdle(uint8_t index) const {
    return index < port_count_ &&
           drivers::UartTransmitIdle(ports_[index].index);
}

int UartPort::Transmit(uint8_t index, const uint8_t* data, size_t length,
                       uint32_t timeout_us) {
    if (index >= port_count_ || !data || length > 512) {
        return -EINVAL;
    }
    atomic_set(&ports_[index].busy, 1);
    const int result = drivers::UartTransmit(ports_[index].index, data, length,
                                             timeout_us);
    if (result) {
        atomic_clear(&ports_[index].busy);
    }
    return result;
}

int UartPort::Abort(uint8_t index) {
    if (index >= port_count_) {
        return -ENODEV;
    }
    atomic_set(&ports_[index].cancelling, 1);
    const int result = drivers::UartAbort(ports_[index].index);
    if (result) {
        atomic_clear(&ports_[index].cancelling);
    }
    return result;
}

bool UartPort::TakeRx(uint8_t index, UartRxBlock& out) {
    if (index >= port_count_) {
        return false;
    }
    return k_msgq_get(&ports_[index].rx, &out, K_NO_WAIT) == 0;
}

void UartPort::SetRxEpoch(uint32_t epoch) {
    atomic_set(&epoch_, epoch);
}

UartStats UartPort::Stats(uint8_t index) const {
    if (index >= port_count_) {
        return {};
    }
    const auto& port = ports_[index];
    UartStats stats;
    stats.received = uint32_t(atomic_get(&port.received));
    stats.drops = uint32_t(atomic_get(&port.drops));
    stats.completed = uint32_t(atomic_get(&port.completed));
    stats.errors = uint32_t(atomic_get(&port.errors));
    stats.busy = atomic_get(&port.busy) != 0;
    stats.queued = uint32_t(k_msgq_num_used_get(
        const_cast<k_msgq*>(&port.rx)));
    return stats;
}

bool UartPort::Faulted(uint8_t index) const {
    return index < port_count_ && atomic_get(&ports_[index].fault) != 0;
}

}  // namespace ecat
