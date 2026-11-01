/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 总线后端：中断接收队列、批次发送、保护帧、反馈快照与状态查询。 */

#include "ecat/io_internal.hpp"

#include "drivers/can_driver.hpp"
#include "ecat/can_profile.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace dust::ecat {

CanPort buses[] = {{DEVICE_DT_GET(DT_ALIAS(dust_can_0)), 0},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_1)), 1},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_2)), 2},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_3)), 3}};

uint64_t source_deadlines[2]{};

namespace {
std::array<drivers::CanFrame, 16> hardware_frames;
std::array<PendingCan, 16> admitted;

void CanDone(void*, uint8_t bus_index, int error) {
    auto& bus = buses[bus_index];
    if (error && error != -ECANCELED && error != -ENETDOWN) {
        atomic_inc(&bus.errors);
        atomic_set(&bus.fault, 1);
    } else if (!error) {
        atomic_inc(&bus.completed);
    }
    atomic_clear(&bus.busy);
}

void CanReceive(void*, uint8_t bus_index, const drivers::CanFrame& frame) {
    auto& bus = buses[bus_index];
    const uint32_t sequence = bus.rx_sequence++;
    atomic_inc(&bus.received);
    if (frame.length > 8 || frame.id > 0x7ff) {
        atomic_inc(&bus.drops);
        return;
    }
    Core::CanEvent value;
    value.id = frame.id;
    value.sequence = sequence;
    value.capture_us = uint32_t(Now());
    value.length = frame.length;
    std::memcpy(value.data.data(), frame.data, frame.length);
    int queued = k_msgq_put(&bus.receive, &value, K_NO_WAIT);
    if (queued) {
        Core::CanEvent discarded;
        (void)k_msgq_get(&bus.receive, &discarded, K_NO_WAIT);
        atomic_inc(&bus.drops);
        queued = k_msgq_put(&bus.receive, &value, K_NO_WAIT);
        if (queued) {
            atomic_inc(&bus.drops);
        }
    }
}
}  // namespace

int CanInitialize() {
    drivers::CanBusConfig configs[4]{};
    drivers::CanFilterConfig filters[64]{};
    size_t bus_count = 0, filter_count = 0;
    for (const auto& configured : dust::can_profile::kBuses) {
        auto& bus = buses[configured.channel];
        configs[bus_count++] = {bus.dev, bus.index, configured.bitrate};
        for (const auto& address : dust::can_profile::kReceiveFilters) {
            if (address.channel != bus.index) {
                continue;
            }
            if (address.flags || address.id > 0x7ff) {
                return -EINVAL;
            }
            filters[filter_count++] = {bus.index, address.id, 0x7ff};
        }
    }
    const int result = drivers::CanInit(configs, bus_count, filters,
                                        filter_count, CanReceive, nullptr);
    if (result) {
        return result;
    }
    for (size_t i = 0; i < bus_count; ++i) {
        buses[configs[i].index].ready =
            drivers::CanReady(configs[i].index);
    }
    return 0;
}

void CanStop() {
    for (auto& bus : buses) {
        bus.transmit.Clear();
        if (bus.feedback_count) ++bus.batch;
        bus.feedback_count = 0;
        (void)bus.latest.Configure(nullptr, 0);
        if (!bus.ready) {
            continue;
        }
        if (drivers::CanAbortTx(bus.index)) {
            ++protection_errors;
            // 撤销未确认时直接停止控制器，不能继续提交业务或假定旧请求已消失。
            drivers::CanStopController(bus.index);
            bus.ready = false;
        }
        atomic_clear(&bus.busy);
        bus.deadline = 0;
    }
    for (const auto& stop : dust::can_profile::kProtectionFrames) {
        if (!buses[stop.channel].ready) {
            ++protection_errors;
            continue;
        }
        PendingCan value;
        value.frame.bus = stop.channel;
        value.frame.id = stop.id;
        value.frame.length = stop.length;
        std::memcpy(value.frame.data.data(), stop.data, stop.length);
        if (!buses[stop.channel].transmit.Push(value)) {
            ++protection_errors;
        }
    }
}

void CanPump(Core* core, uint64_t now) {
    for (auto& bus : buses) {
        if (!bus.ready) {
            continue;
        }
        if (core && atomic_get(&bus.fault) &&
            core->state() != BusinessState::Unconfigured &&
            core->state() != BusinessState::Protected) {
            core->Protect(Error::Transport);
            return;
        }
        if (atomic_get(&bus.busy)) {
            if (bus.deadline && now >= bus.deadline) {
                if (core) {
                    core->Protect(Error::Lease);
                }
                return;
            }
            continue;
        }
        if (!bus.transmit.size()) {
            continue;
        }

        const unsigned count = std::min(size_t(16), bus.transmit.size());
        uint64_t deadline = 0;
        for (unsigned i = 0; i < count; ++i) {
            admitted[i] = *bus.transmit.At(i);
            if (CanExpired(admitted[i], now)) {
                if (core) {
                    core->Protect(Error::Lease);
                }
                return;
            }
            if (admitted[i].deadline_us) {
                deadline = deadline
                               ? std::min(deadline, admitted[i].deadline_us)
                               : admitted[i].deadline_us;
            }
            auto& target = hardware_frames[i];
            target = {};
            target.id = admitted[i].frame.id;
            target.length = admitted[i].frame.length;
            std::memcpy(target.data, admitted[i].frame.data.data(),
                        target.length);
        }
        bus.deadline = deadline;
        atomic_set(&bus.busy, 1);
        const int result = drivers::CanSendBatch(
            bus.index, hardware_frames.data(), count, deadline, CanDone,
            nullptr);
        if (!result) {
            for (unsigned i = 0; i < count; ++i) {
                bus.transmit.Pop();
            }
            atomic_add(&bus.submitted, count);
        } else {
            atomic_clear(&bus.busy);
            if (result != -EAGAIN && result != -EBUSY) {
                atomic_inc(&bus.errors);
                atomic_set(&bus.fault, 1);
                if (core && core->state() != BusinessState::Protected) {
                    core->Protect(result == -ETIMEDOUT ? Error::Lease
                                                       : Error::Transport);
                    return;
                }
                drivers::CanStopController(bus.index);
                bus.ready = false;
                bus.transmit.Clear();
                ++protection_errors;
                continue;
            }
        }
    }
}

bool CanValidate(void*, unsigned, const CanFrame& frame) {
    if (frame.bus >= 4 || !buses[frame.bus].ready || frame.id > 0x7ff ||
        frame.length > 8) {
        return false;
    }
    dust::can_profile::CanFrame record{};
    record.channel = frame.bus;
    record.id = frame.id;
    record.length = frame.length;
    return dust::can_profile::AllowsTransmit(record);
}

void CanApplyLimits(Limits& limits) {
    for (const auto& bus : buses) {
        if (bus.ready) {
            limits.can_mask |= 1U << bus.index;
        }
    }
}

bool CanCapacityValid(const Configuration& value) {
    for (unsigned bus = 0; bus < 4; ++bus) {
        unsigned count = 0;
        for (const auto& filter : dust::can_profile::kReceiveFilters)
            if (filter.channel == bus) ++count;
        if (value.can_rx[bus] && value.can_rx[bus] < count) return false;
    }
    return true;
}

void CanConfigure(const Configuration& value) {
    for (auto& bus : buses) {
        uint32_t ids[LatestCanSlots::kMaxSlots]{};
        size_t count = 0;
        if (value.can_rx[bus.index]) {
            for (const auto& filter : dust::can_profile::kReceiveFilters) {
                if (filter.channel == bus.index &&
                    count < LatestCanSlots::kMaxSlots) {
                    ids[count++] = filter.id;
                }
            }
        }
        (void)bus.latest.Configure(ids, count);
        bus.feedback_count = 0;
    }
}

bool CanDrainReceive() {
    bool changed = false;
    for (auto& bus : buses) {
        bool bus_changed = false;
        Core::CanEvent value;
        for (unsigned drained = 0; drained < 32; ++drained) {
            if (k_msgq_get(&bus.receive, &value, K_NO_WAIT)) break;
            if (uint32_t(uint32_t(Now()) - value.capture_us) >= kTimeoutUs) {
                atomic_inc(&bus.drops);
                continue;
            }
            if (!configuration.can_rx[bus.index]) continue;
            if (bus.latest.Update(value)) bus_changed = true;
            else atomic_inc(&bus.drops);
        }
        if (bus.latest.Expire(uint32_t(Now()), kTimeoutUs)) bus_changed = true;
        if (bus_changed) {
            bus.feedback_count = 0;
            for (size_t i = 0; i < bus.latest.Count(); ++i) {
                if (bus.latest.Valid(i))
                    bus.feedback[bus.feedback_count++] = bus.latest.Frame(i);
            }
            ++bus.batch;
            changed = true;
        }
    }
    return changed;
}

void CanCaptureFeedback(Core::Feedback& feedback) {
    for (auto& bus : buses) {
        feedback.can[bus.index] = {bus.batch, uint32_t(atomic_get(&bus.drops)),
                                   bus.feedback.data(),
                                   uint16_t(bus.feedback_count)};
        can_state state;
        can_bus_err_cnt errors{};
        if (bus.ready && drivers::CanGetState(bus.index, state, errors)) {
            if (state == CAN_STATE_BUS_OFF) {
                atomic_set(&bus.fault, 1);
            }
        }
    }
}

bool CanAnyFault() {
    for (const auto& bus : buses) {
        if (atomic_get(&bus.fault)) {
            return true;
        }
    }
    return false;
}

int CanStatus(unsigned channel, unsigned page,
              std::array<uint32_t, 3>& values) {
    const auto& bus = buses[channel];
    if (!page) {
        values = {uint32_t(atomic_get(&bus.received)),
                  uint32_t(atomic_get(&bus.submitted)),
                  uint32_t(atomic_get(&bus.completed))};
        return 0;
    }
    if (page == 1) {
        values = {uint32_t(atomic_get(&bus.drops)),
                  uint32_t(atomic_get(&bus.errors)),
                  uint32_t(bus.transmit.size())};
        return 0;
    }
    if (page == 2) {
        can_state state;
        can_bus_err_cnt errors{};
        const int result =
            drivers::CanGetState(bus.index, state, errors) ? 0 : -EIO;
        if (!result) {
            values = {uint32_t(state), errors.tx_err_cnt, errors.rx_err_cnt};
        }
        return result;
    }
    return -ENOTSUP;
}

bool CanDispatch(Core& core, const Actions& actions, uint64_t now) {
    unsigned required[4]{};
    for (const auto& batch : actions.can) {
        if (batch.valid) {
            if (!batch.count || batch.count > batch.frames.size() ||
                batch.deadline_us <= now) {
                core.Protect(Error::Wire);
                return false;
            }
            for (unsigned i = 0; i < batch.count; ++i) {
                if (!CanValidate(nullptr, 0, batch.frames[i])) {
                    core.Protect(Error::Protection);
                    return false;
                }
                ++required[batch.frames[i].bus];
            }
        }
    }
    for (unsigned bus = 0; bus < 4; ++bus) {
        if (required[bus] > buses[bus].transmit.free()) {
            core.Protect(Error::Capacity);
            return false;
        }
    }
    for (unsigned source = 0; source < 2; ++source) {
        const auto& batch = actions.can[source];
        if (!batch.valid) {
            continue;
        }
        const uint64_t deadline = std::min(
            batch.deadline_us, now + dust::can_profile::kCommandTimeoutUs);
        source_deadlines[source] = deadline;
        for (unsigned i = 0; i < batch.count; ++i) {
            if (!buses[batch.frames[i].bus].transmit.Push(
                    {batch.frames[i], deadline})) {
                core.Protect(Error::Capacity);
                return false;
            }
        }
    }
    return true;
}

}  // namespace dust::ecat
