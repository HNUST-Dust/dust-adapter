/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/can/can_bus.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

namespace ecat {

CanBusPort::CanBusPort(const CanBusConfig* buses, size_t count) {
    bus_count_ = std::min(count, kBusCount);
    for (size_t i = 0; i < bus_count_; ++i) {
        buses_[i].dev = static_cast<const device*>(buses[i].device);
        buses_[i].index = buses[i].index;
        buses_[i].bitrate = buses[i].bitrate;
        atomic_clear(&buses_[i].busy);
        atomic_clear(&buses_[i].received);
        atomic_clear(&buses_[i].drops);
        atomic_clear(&buses_[i].completed);
        atomic_clear(&buses_[i].errors);
        k_msgq_init(&buses_[i].rx, buses_[i].storage, sizeof(CanSample),
                    kRxDepth);
    }
}

void CanBusPort::RxTrampoline(void* context, uint8_t bus,
                              const drivers::CanFrame& frame) {
    auto* self = static_cast<CanBusPort*>(context);
    if (bus >= self->bus_count_) {
        return;
    }
    auto& port = self->buses_[bus];
    if (frame.length > 8 || frame.id > 0x7ff) {
        atomic_inc(&port.drops);
        return;
    }
    CanSample value;
    value.id = frame.id;
    value.length = frame.length;
    value.sequence = uint32_t(atomic_inc(&port.received));
    value.capture_us = uint32_t(k_ticks_to_us_floor64(k_uptime_ticks()));
    std::memcpy(value.data.data(), frame.data, frame.length);
    if (k_msgq_put(&port.rx, &value, K_NO_WAIT)) {
        CanSample discarded;
        (void)k_msgq_get(&port.rx, &discarded, K_NO_WAIT);
        atomic_inc(&port.drops);
        if (k_msgq_put(&port.rx, &value, K_NO_WAIT)) {
            atomic_inc(&port.drops);
        }
    }
}

void CanBusPort::TxDoneTrampoline(void* context, uint8_t bus, int error) {
    auto* self = static_cast<CanBusPort*>(context);
    if (bus >= self->bus_count_) {
        return;
    }
    auto& port = self->buses_[bus];
    if (error && error != -ECANCELED && error != -ENETDOWN) {
        atomic_inc(&port.errors);
    } else if (!error) {
        atomic_inc(&port.completed);
    }
    atomic_clear(&port.busy);
}

int CanBusPort::Init(const CanFilterConfig* filters, size_t count) {
    drivers::CanBusConfig configs[kBusCount]{};
    drivers::CanFilterConfig driver_filters[64]{};
    if (count > 64) {
        return -EINVAL;
    }
    for (size_t i = 0; i < bus_count_; ++i) {
        configs[i] = {buses_[i].dev, buses_[i].index, buses_[i].bitrate};
    }
    for (size_t i = 0; i < count; ++i) {
        driver_filters[i] = {filters[i].bus, filters[i].id, filters[i].mask};
    }
    const int result = drivers::CanInit(configs, bus_count_, driver_filters,
                                        count, RxTrampoline, this);
    if (result) {
        return result;
    }
    for (size_t i = 0; i < bus_count_; ++i) {
        buses_[i].ready = drivers::CanReady(buses_[i].index);
    }
    return 0;
}

bool CanBusPort::Ready(uint8_t bus) const {
    return bus < bus_count_ && buses_[bus].ready;
}

int CanBusPort::SubmitBatch(uint8_t bus, const CanFrame* frames, size_t count,
                            uint64_t deadline_us) {
    if (bus >= bus_count_ || !buses_[bus].ready || count > 16) {
        return -ENODEV;
    }
    std::array<drivers::CanFrame, 16> out{};
    for (size_t i = 0; i < count; ++i) {
        out[i].id = frames[i].id;
        out[i].length = frames[i].length;
        std::memcpy(out[i].data, frames[i].data.data(), frames[i].length);
    }
    atomic_set(&buses_[bus].busy, 1);
    const int result = drivers::CanSendBatch(
        bus, out.data(), count, deadline_us, TxDoneTrampoline, this);
    if (result) {
        atomic_clear(&buses_[bus].busy);
    }
    return result;
}

int CanBusPort::Abort(uint8_t bus) {
    return bus < bus_count_ ? drivers::CanAbortTx(bus) : -ENODEV;
}

int CanBusPort::StopController(uint8_t bus) {
    if (bus >= bus_count_) {
        return -ENODEV;
    }
    buses_[bus].ready = false;
    drivers::CanStopController(bus);
    return 0;
}

bool CanBusPort::GetState(uint8_t bus, uint32_t& state, uint32_t& tx_err,
                          uint32_t& rx_err) {
    if (bus >= bus_count_) {
        return false;
    }
    can_state can_state_value;
    can_bus_err_cnt errors{};
    if (!drivers::CanGetState(bus, can_state_value, errors)) {
        return false;
    }
    state = uint32_t(can_state_value);
    tx_err = errors.tx_err_cnt;
    rx_err = errors.rx_err_cnt;
    return true;
}

bool CanBusPort::TakeRx(uint8_t bus, CanSample& out) {
    if (bus >= bus_count_) {
        return false;
    }
    return k_msgq_get(&buses_[bus].rx, &out, K_NO_WAIT) == 0;
}

CanBusStats CanBusPort::Stats(uint8_t bus) const {
    if (bus >= bus_count_) {
        return {};
    }
    const auto& port = buses_[bus];
    CanBusStats stats;
    stats.received = uint32_t(atomic_get(&port.received));
    stats.drops = uint32_t(atomic_get(&port.drops));
    stats.completed = uint32_t(atomic_get(&port.completed));
    stats.errors = uint32_t(atomic_get(&port.errors));
    stats.busy = atomic_get(&port.busy) != 0;
    return stats;
}

}  // namespace ecat
