/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/can/can_bus.hpp"
#include "common/abi.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <zephyr/irq.h>
#include "common/device_pdo.hpp"

namespace ecat {







namespace {
struct MotorRecord {
    dust::device_pdo::Record bytes{};
    uint32_t capture_us = 0;
    bool valid = false;
};
std::array<MotorRecord, 6> latest_motor_records{};
}
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
namespace {
struct Arrival {
    uint32_t previous_cycle = 0, count = 0;
    uint32_t min_irq = UINT32_MAX, max_irq = 0;
    uint32_t min_wire = UINT32_MAX, max_wire = 0, max_age = 0;
    uint16_t previous_wire = 0;
    bool have_previous = false;
};
std::array<Arrival, 6> arrivals{};
bool arrival_test_active;
void ObserveArrival(unsigned index, uint16_t timestamp, uint16_t age) {
    if (!arrival_test_active) return;
    auto& a = arrivals[index];
    const auto cycle = k_cycle_get_32();
    a.max_age = std::max(a.max_age, uint32_t(age));
    if (a.have_previous) {
        const auto irq_us = k_cyc_to_us_floor32(cycle - a.previous_cycle);
        // Hardware counter is 16 us/tick at 1 Mbps. Reject ambiguous wraps.
        const uint32_t wire_ticks = irq_us < 1048576U
            ? uint16_t(timestamp - a.previous_wire) : UINT32_MAX;
        ++a.count;
        a.min_irq = std::min(a.min_irq, irq_us);
        a.max_irq = std::max(a.max_irq, irq_us);
        a.min_wire = std::min(a.min_wire, wire_ticks);
        a.max_wire = std::max(a.max_wire, wire_ticks);
    }
    a.previous_cycle = cycle;
    a.previous_wire = timestamp;
    a.have_previous = true;
}
}
void BeginMotorArrivalTest() {
    const auto key = irq_lock();
    arrivals = {};
    arrival_test_active = true;
    irq_unlock(key);
}
void FreezeMotorArrivalTest(uint32_t (&words)[36]) {
    const auto key = irq_lock();
    arrival_test_active = false;
    for (unsigned i = 0; i < arrivals.size(); ++i) {
        const auto& a = arrivals[i];
        words[30 + i] = a.max_age;
        words[i * 5] = a.count;
        words[i * 5 + 1] = a.count ? a.min_irq : 0;
        words[i * 5 + 2] = a.max_irq;
        words[i * 5 + 3] = a.count ? a.min_wire : 0;
        words[i * 5 + 4] = a.max_wire;
    }
    irq_unlock(key);
}
#endif

void CopyLatestMotorRecords(uint8_t* destination, uint32_t now_us) {
    const auto key = irq_lock();
    for (unsigned i = 0; i < latest_motor_records.size(); ++i) {
        const auto& sample = latest_motor_records[i];
        if (sample.valid && uint32_t(now_us - sample.capture_us) < kTimeoutUs) {
            std::copy(sample.bytes.begin(), sample.bytes.end(), destination + i * 10);
        }
    }
    irq_unlock(key);
}

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
    if (frame.length == 8) {
        namespace wire = dust::device_pdo;
        for (unsigned i = 0; i < wire::kRoutes.size(); ++i) {
            const auto& route = wire::kRoutes[i];
            if (route.bus == bus && route.feedback == frame.id) {
                auto& latest = latest_motor_records[i];
                wire::Payload payload;
                std::copy_n(frame.data, 8, payload.begin());
                wire::EncodeMotor(wire::Device(i), uint8_t(value.sequence),
                                  payload, latest.bytes);
                latest.capture_us = value.capture_us;
                latest.valid = true;
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
                ObserveArrival(i, frame.arrival_timestamp, frame.arrival_age_ticks);
#endif
                // DTG1 consumes this complete record directly in InputMapping.
                // Do not also queue/rebuild the same sample through legacy feedback.
                return;
            }
        }
    }
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
