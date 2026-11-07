/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 业务服务：白名单、批次租约、保护帧、反馈槽与统计。
 * 通过 drivers::CanBusPort 访问硬件。 */

#include "domain/can/can_service.hpp"

#include <algorithm>
#include <cerrno>

namespace ecat {

CanService::CanService(CanBusPort& bus, const CanTopology& topology)
    : bus_(bus), topology_(topology) {}

void CanService::AttachFaultSink(FaultSink sink) {
    fault_sink_ = sink;
}

int CanService::Initialize() {
    if (!topology_.filters || !topology_.tx_addresses) {
        return -EINVAL;
    }
    for (size_t i = 0; i < topology_.tx_count; ++i) {
        const auto& address = topology_.tx_addresses[i];
        bool protected_address = false;
        for (size_t p = 0; p < topology_.protection_count; ++p) {
            const auto& stop = topology_.protection_frames[p];
            protected_address |= stop.bus == address.bus &&
                                 stop.id == address.id;
        }
        if (!protected_address || address.flags || address.id > 0x7ff) {
            return -EACCES;
        }
    }
    for (size_t i = 0; i < topology_.filter_count; ++i) {
        const auto& filter = topology_.filters[i];
        if (filter.bus >= kBusCount || filter.id > 0x7ff ||
            filter.mask != 0x7ff) {
            return -EINVAL;
        }
    }
    if (topology_.command_timeout_us > kTimeoutUs) {
        return -EINVAL;
    }
    const int result = bus_.Init(topology_.filters, topology_.filter_count);
    if (result) {
        return result;
    }
    for (unsigned index = 0; index < kBusCount; ++index) {
        ports_[index].ready = bus_.Ready(uint8_t(index));
    }
    return 0;
}

void CanService::ApplyLimits(Limits& limits) const {
    for (unsigned index = 0; index < kBusCount; ++index) {
        if (ports_[index].ready) {
            limits.can_mask |= 1U << index;
        }
    }
}

bool CanService::CapacityValid(const Configuration& value) const {
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        unsigned count = 0;
        for (size_t i = 0; i < topology_.filter_count; ++i) {
            if (topology_.filters[i].bus == bus) {
                ++count;
            }
        }
        if (value.can_rx[bus] && value.can_rx[bus] < count) {
            return false;
        }
    }
    return true;
}

void CanService::Configure(const Configuration& value) {
    can_rx_enabled_ = value.can_rx;
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        uint32_t ids[LatestCanSlots::kMaxSlots]{};
        size_t count = 0;
        if (value.can_rx[bus]) {
            for (size_t i = 0; i < topology_.filter_count; ++i) {
                if (topology_.filters[i].bus == bus &&
                    count < LatestCanSlots::kMaxSlots) {
                    ids[count++] = topology_.filters[i].id;
                }
            }
        }
        (void)ports_[bus].latest.Configure(ids, count);
        ports_[bus].feedback_count = 0;
    }
}

void CanService::EnterActive(uint32_t active_sources, uint64_t now) {
    for (unsigned source = 0; source < 2; ++source) {
        source_deadlines_[source] =
            active_sources & (1U << source)
                ? now + topology_.command_timeout_us
                : 0;
    }
}

bool CanService::LeaseExpired(uint64_t now) const {
    for (auto deadline : source_deadlines_) {
        if (deadline && now >= deadline) {
            return true;
        }
    }
    return false;
}

void CanService::ClearAndAbort() {
    source_deadlines_ = {};
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        auto& port = ports_[bus];
        port.transmit.Clear();
        if (port.feedback_count) {
            ++port.batch;
        }
        port.feedback_count = 0;
        (void)port.latest.Configure(nullptr, 0);
        port.deadline = 0;
        if (!port.ready) {
            continue;
        }
        if (bus_.Abort(uint8_t(bus))) {
            ++port.protection_errors;
            /* 撤销未确认时直接停控制器，不能继续提交业务或假定旧请求已消失。 */
            (void)bus_.StopController(uint8_t(bus));
            port.ready = false;
        }
    }
}

void CanService::EmitProtectionFrames() {
    for (size_t i = 0; i < topology_.protection_count; ++i) {
        const auto& frame = topology_.protection_frames[i];
        auto& port = ports_[frame.bus];
        if (!port.ready) {
            ++port.protection_errors;
            continue;
        }
        PendingCan value;
        value.frame = frame;
        if (!port.transmit.Push(value)) {
            ++port.protection_errors;
        }
    }
}

void CanService::Stop() {
    ClearAndAbort();
    EmitProtectionFrames();
}

void CanService::Pump(uint64_t now) {
    for (unsigned index = 0; index < kBusCount; ++index) {
        auto& port = ports_[index];
        if (!port.ready) {
            continue;
        }
        const CanBusStats stats = bus_.Stats(uint8_t(index));
        if (stats.errors > port.errors_seen) {
            port.errors_seen = stats.errors;
            port.fault = true;
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Transport);
            }
            return;
        }
        if (stats.busy) {
            if (port.deadline && now >= port.deadline) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::Lease);
                }
                return;
            }
            continue;
        }
        if (!port.transmit.size()) {
            continue;
        }

        const unsigned count = std::min(size_t(kMaxBatch), port.transmit.size());
        std::array<CanFrame, kMaxBatch> frames{};
        uint64_t deadline = 0;
        for (unsigned i = 0; i < count; ++i) {
            const PendingCan admitted = *port.transmit.At(i);
            if (CanExpired(admitted, now)) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::Lease);
                }
                return;
            }
            if (admitted.deadline_us) {
                deadline = deadline ? std::min(deadline, admitted.deadline_us)
                                    : admitted.deadline_us;
            }
            frames[i] = admitted.frame;
        }
        port.deadline = deadline;
        const int result =
            bus_.SubmitBatch(uint8_t(index), frames.data(), count, deadline);
        if (!result) {
            for (unsigned i = 0; i < count; ++i) {
                port.transmit.Pop();
            }
            port.submitted += count;
            continue;
        }
        port.deadline = 0;
        if (result == -EAGAIN || result == -EBUSY) {
            continue;
        }
        port.fault = true;
        port.errors_seen = bus_.Stats(uint8_t(index)).errors;
        if (fault_sink_.report) {
            fault_sink_.Report(result == -ETIMEDOUT ? Error::Lease
                                                     : Error::Transport);
        }
        (void)bus_.StopController(uint8_t(index));
        port.ready = false;
        port.transmit.Clear();
        ++port.protection_errors;
    }
}

bool CanService::AnyFault() const {
    for (const auto& port : ports_) {
        if (port.fault) {
            return true;
        }
    }
    return false;
}

bool CanService::DrainReceive(uint64_t now_us) {
    bool changed = false;
    const uint32_t now = uint32_t(now_us);
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        auto& port = ports_[bus];
        bool bus_changed = false;
        CanSample value;
        for (unsigned drained = 0; drained < 32; ++drained) {
            if (!bus_.TakeRx(uint8_t(bus), value)) {
                break;
            }
            if (int32_t(now - value.capture_us) >= int32_t(kTimeoutUs)) {
                ++port.drain_drops;
                continue;
            }
            if (!can_rx_enabled_[bus]) {
                continue;
            }
            if (port.latest.Update(value)) {
                bus_changed = true;
            } else {
                ++port.drain_drops;
            }
        }
        if (port.latest.Expire(now, kTimeoutUs)) {
            bus_changed = true;
        }
        if (!bus_changed) {
            continue;
        }
        port.feedback_count = 0;
        for (size_t i = 0; i < port.latest.Count(); ++i) {
            if (port.latest.Valid(i)) {
                port.feedback[port.feedback_count++] = port.latest.Frame(i);
            }
        }
        ++port.batch;
        changed = true;
    }
    return changed;
}

void CanService::CaptureFeedback(Feedback& feedback) {
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        auto& port = ports_[bus];
        const CanBusStats stats = bus_.Stats(uint8_t(bus));
        feedback.can[bus] = {port.batch,
                             stats.drops + port.drain_drops,
                             port.feedback.data(),
                             uint16_t(port.feedback_count)};
        uint32_t state = 0, tx_err = 0, rx_err = 0;
        if (port.ready && bus_.GetState(uint8_t(bus), state, tx_err, rx_err)) {
            if (state == 2U /* CAN_STATE_BUS_OFF */) {
                port.fault = true;
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::Transport);
                }
            }
        }
    }
}

bool CanService::Dispatch(const Actions& actions, uint64_t now) {
    unsigned required[4]{};
    for (const auto& batch : actions.can) {
        if (!batch.valid) {
            continue;
        }
        if (batch.count > batch.frames.size() ||
            batch.deadline_us <= now) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Wire);
            }
            return false;
        }
        for (unsigned i = 0; i < batch.count; ++i) {
            const auto& frame = batch.frames[i];
            if (frame.bus >= kBusCount || !ports_[frame.bus].ready ||
                frame.id > 0x7ff || frame.length > 8 ||
                !topology_.AllowsTransmit(frame)) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::Protection);
                }
                return false;
            }
            ++required[frame.bus];
        }
    }
    for (unsigned bus = 0; bus < kBusCount; ++bus) {
        if (required[bus] > ports_[bus].transmit.free()) {
            if (fault_sink_.report) {
                fault_sink_.Report(Error::Capacity);
            }
            return false;
        }
    }
    for (unsigned source = 0; source < actions.can.size(); ++source) {
        const auto& batch = actions.can[source];
        if (!batch.valid) {
            continue;
        }
        const uint64_t deadline =
            std::min(batch.deadline_us, now + topology_.command_timeout_us);
        source_deadlines_[source] = deadline;
        for (unsigned i = 0; i < batch.count; ++i) {
            if (!ports_[batch.frames[i].bus].transmit.Push(
                    {batch.frames[i], deadline})) {
                if (fault_sink_.report) {
                    fault_sink_.Report(Error::Capacity);
                }
                return false;
            }
        }
    }
    return true;
}

int CanService::Status(unsigned channel, unsigned page,
                         std::array<uint32_t, 3>& values) const {
    if (channel >= kBusCount) {
        return -EINVAL;
    }
    const auto& port = ports_[channel];
    const CanBusStats stats = bus_.Stats(uint8_t(channel));
    if (!page) {
        values = {stats.received, port.submitted, stats.completed};
        return 0;
    }
    if (page == 1) {
        values = {stats.drops + port.drain_drops, stats.errors,
                  uint32_t(port.transmit.size())};
        return 0;
    }
    if (page == 2) {
        uint32_t state = 0, tx_err = 0, rx_err = 0;
        if (!bus_.GetState(uint8_t(channel), state, tx_err, rx_err)) {
            return -EIO;
        }
        values = {state, tx_err, rx_err};
        return 0;
    }
    return -ENOTSUP;
}

uint32_t CanService::ProtectionErrors() const {
    uint32_t total = 0;
    for (const auto& port : ports_) {
        total += port.protection_errors;
    }
    return total;
}

bool CanService::ValidateFrame(const CanFrame& frame) const {
    if (frame.bus >= kBusCount || !ports_[frame.bus].ready ||
        frame.id > 0x7ff || frame.length > 8) {
        return false;
    }
    return topology_.AllowsTransmit(frame);
}

int CanService::CopyProtectionFrames(uint8_t bus, CanFrame* frames,
                                     size_t capacity) const {
    if (bus >= kBusCount || !frames) {
        return -EINVAL;
    }
    size_t count = 0;
    for (size_t i = 0; i < topology_.protection_count; ++i) {
        count += topology_.protection_frames[i].bus == bus;
    }
    if (count > capacity) {
        return -ENOSPC;
    }
    size_t index = 0;
    for (size_t i = 0; i < topology_.protection_count; ++i) {
        const auto& frame = topology_.protection_frames[i];
        if (frame.bus == bus) {
            frames[index++] = frame;
        }
    }
    return int(count);
}

uint32_t CanService::QueuedFrames(uint8_t bus) const {
    return bus < kBusCount ? uint32_t(ports_[bus].transmit.size()) : 0;
}

}  // namespace ecat
