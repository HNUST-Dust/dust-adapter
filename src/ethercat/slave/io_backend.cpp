/* SPDX-License-Identifier: Apache-2.0 */
#include "io_backend.hpp"

#include <drivers/can/hpmicro_mcan_batch.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/atomic.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "drivers/firmware_can_profile.hpp"
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
#include "drivers/imu_sensor.hpp"
#endif
#include "drivers/uart_receive.hpp"
#include "ethercat/io_policy.hpp"
#include "ethercat/latest_can.hpp"

namespace dust::ecat {
int AllocateBootIdentity(uint64_t& boot);
namespace {
uint64_t Now() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}
struct CanPort {
    const device* dev;
    uint8_t index;
    k_msgq receive;
    alignas(4) char storage[32 * sizeof(Core::CanEvent)];
    IoQueue<PendingCan, 32> transmit;
    std::array<Core::CanEvent, 32> feedback{};
    LatestCanSlots latest;
    uint32_t batch = 0, rx_sequence = 0;
    unsigned feedback_count = 0;
    uint64_t deadline = 0;
    atomic_t busy{}, fault{}, received{}, submitted{}, completed{}, errors{},
        drops{};
    bool ready = false;
};
CanPort buses[] = {{DEVICE_DT_GET(DT_ALIAS(dust_can_0)), 0},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_1)), 1},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_2)), 2},
                   {DEVICE_DT_GET(DT_ALIAS(dust_can_3)), 3}};
struct UartBlock {
    uint32_t epoch = 0, captured = 0;
    uint16_t size = 0;
    bool discontinuity = false;
    std::array<uint8_t, 128> data{};
};
struct UartWrite {
    uint64_t accepted = 0;
    uint16_t size = 0;
    std::array<uint8_t, 512> data{};
};
struct UartPort {
    const device* dev;
    uint8_t index;
    k_msgq receive;
    alignas(4) char storage[4 * sizeof(UartBlock)];
    IoQueue<UartWrite, 4> transmit;
    UartBlock pending{};
    uint16_t offset = 0;
    uint8_t next_buffer = 1;
    bool ready = false, discontinuity = false;
    uint64_t started = 0;
    atomic_t busy{}, fault{}, drops{}, received{}, completed{}, errors{}, gap{},
        cancelling{};
};
UartPort ports[] = {{DEVICE_DT_GET(DT_ALIAS(dust_uart_1)), 0},
                    {DEVICE_DT_GET(DT_ALIAS(dust_uart_2)), 1},
                    {DEVICE_DT_GET(DT_ALIAS(dust_uart_3)), 2},
                    {DEVICE_DT_GET(DT_ALIAS(dust_uart_4)), 3},
                    {DEVICE_DT_GET(DT_ALIAS(dust_uart_5)), 4},
                    {DEVICE_DT_GET(DT_ALIAS(dust_uart_6)), 5}};
__nocache __aligned(4) uint8_t rx_buffers[6][2][128];
__nocache __aligned(4) uint8_t tx_buffers[6][512];
atomic_t capture_epoch{};
uint32_t epoch = 0;
bool initialized = false, ready = false, capturing = false;
uint32_t protection_errors = 0;
uint64_t source_deadlines[2]{};
Configuration configuration;
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
modules::ImuSensor onboard_imu;
bool imu_ready = false;
uint32_t imu_sequence = 0;
#endif
std::array<can_frame, 16> hardware_frames;
std::array<PendingCan, 16> admitted;

void CanDone(const device*, int error, void* user) {
    auto& bus = *static_cast<CanPort*>(user);
    if (error && error != -ECANCELED && error != -ENETDOWN) {
        atomic_inc(&bus.errors);
        atomic_set(&bus.fault, 1);
    } else if (!error) {
        atomic_inc(&bus.completed);
    }
    atomic_clear(&bus.busy);
}
void CanReceive(const device*, can_frame* frame, void* user) {
    auto& bus = *static_cast<CanPort*>(user);
    const uint32_t sequence = bus.rx_sequence++;
    atomic_inc(&bus.received);
    if (frame->dlc > 8 || frame->flags || frame->id > 0x7ff) {
        atomic_inc(&bus.drops);
        return;
    }
    Core::CanEvent value;
    value.id = frame->id;
    value.sequence = sequence;
    value.capture_us = uint32_t(Now());
    value.length = frame->dlc;
    std::memcpy(value.data.data(), frame->data, frame->dlc);
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
void UartEvent(const device* dev, uart_event* event, void* user) {
    auto& port = *static_cast<UartPort*>(user);
    switch (event->type) {
        case UART_TX_DONE:
        case UART_TX_ABORTED:
            if (event->type == UART_TX_DONE) {
                atomic_inc(&port.completed);
                atomic_clear(&port.cancelling);
            } else if (!atomic_cas(&port.cancelling, 1, 0)) {
                atomic_inc(&port.errors);
                atomic_set(&port.fault, 1);
            }
            atomic_clear(&port.busy);
            break;
        case UART_RX_RDY: {
            const uint32_t current = atomic_get(&capture_epoch);
            if (!current || !event->data.rx.len) {
                break;
            }
            UartBlock block;
            block.epoch = current;
            block.captured = uint32_t(Now());
            if (event->data.rx.len > block.data.size()) {
                atomic_inc(&port.drops);
                atomic_set(&port.gap, 1);
                break;
            }
            block.size = event->data.rx.len;
            block.discontinuity = atomic_get(&port.gap) != 0;
            std::memcpy(block.data.data(),
                        event->data.rx.buf + event->data.rx.offset, block.size);
            atomic_inc(&port.received);
            if (k_msgq_put(&port.receive, &block, K_NO_WAIT)) {
                atomic_inc(&port.drops);
                atomic_set(&port.gap, 1);
            } else {
                atomic_clear(&port.gap);
            }
            break;
        }
        case UART_RX_BUF_REQUEST: {
            const unsigned next = port.next_buffer;
            port.next_buffer ^= 1;
            if (uart_rx_buf_rsp(dev, rx_buffers[port.index][next], 128)) {
                atomic_set(&port.gap, 1);
                atomic_inc(&port.drops);
            }
            break;
        }
        case UART_RX_STOPPED:
            atomic_set(&port.gap, 1);
            atomic_inc(&port.drops);
            break;
        case UART_RX_DISABLED:
            port.next_buffer = 1;
            if (uart_rx_enable(dev, rx_buffers[port.index][0], 128, 200)) {
                atomic_set(&port.fault, 1);
            }
            break;
        default:
            break;
    }
}
bool Validate(void*, unsigned, const CanFrame& frame) {
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
void Stop(void*, Error) {
    atomic_clear(&capture_epoch);
    capturing = false;
    for (auto& port : ports) {
        port.transmit.Clear();
        port.pending.size = 0;
        port.offset = 0;
        k_msgq_purge(&port.receive);
        if (port.ready && atomic_get(&port.busy)) {
            atomic_set(&port.cancelling, 1);
            if (uart_tx_abort(port.dev)) {
                atomic_clear(&port.cancelling);
                ++protection_errors;
            }
        }
    }
    for (auto& bus : buses) {
        bus.transmit.Clear();
        if (bus.feedback_count) ++bus.batch;
        bus.feedback_count = 0;
        (void)bus.latest.Configure(nullptr, 0);
        if (!bus.ready) {
            continue;
        }
        if (hpm_mcan_abort_tx(bus.dev, 2000)) {
            ++protection_errors;
            // 撤销未确认时直接停止控制器，不能继续提交业务或假定旧请求已消失。
            (void)can_stop(bus.dev);
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
void EnterActive(Core& core, uint64_t now) {
    if (capturing) {
        return;
    }
    for (auto& port : ports) {
        k_msgq_purge(&port.receive);
        port.pending.size = 0;
        port.offset = 0;
        port.discontinuity = false;
        atomic_clear(&port.gap);
    }
    if (++epoch == 0 || epoch > INT32_MAX) {
        epoch = 1;
    }
    for (unsigned source = 0; source < 2; ++source) {
        source_deadlines[source] =
            core.active_sources() & (1U << source)
                ? now + dust::can_profile::kCommandTimeoutUs
                : 0;
    }
    capturing = true;
    atomic_set(&capture_epoch, epoch);
}
void PumpCan(Core* core, uint64_t now) {
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
            target.dlc = admitted[i].frame.length;
            std::memcpy(target.data, admitted[i].frame.data.data(), target.dlc);
        }
        bus.deadline = deadline;
        atomic_set(&bus.busy, 1);
        const int result =
            deadline ? hpm_mcan_send_batch_until(
                           bus.dev, hardware_frames.data(), count, K_NO_WAIT,
                           deadline, CanDone, &bus)
                     : hpm_mcan_send_batch(bus.dev, hardware_frames.data(),
                                           count, K_NO_WAIT, CanDone, &bus);
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
                (void)can_stop(bus.dev);
                bus.ready = false;
                bus.transmit.Clear();
                ++protection_errors;
                continue;
            }
        }
    }
}
}  // namespace
int InitializeIo(uint64_t& boot) {
    const bool first = !initialized;
    if (!initialized) {
        for (auto& bus : buses) {
            k_msgq_init(&bus.receive, bus.storage, sizeof(Core::CanEvent), 32);
        }
        for (auto& port : ports) {
            k_msgq_init(&port.receive, port.storage, sizeof(UartBlock), 4);
        }
        const auto fail = [](int error) {
            Stop(nullptr, Error::Transport);
            PumpCan(nullptr, Now());
            return error;
        };
        for (const auto& address : dust::can_profile::kTransmitAddresses) {
            bool protected_address = false;
            for (const auto& stop : dust::can_profile::kProtectionFrames) {
                protected_address |= stop.channel == address.channel &&
                                     stop.id == address.id &&
                                     stop.flags == address.flags;
            }
            if (!protected_address || address.flags || address.id > 0x7ff) {
                return fail(-EACCES);
            }
        }
        if (dust::can_profile::kCommandTimeoutUs > kTimeoutUs) {
            return fail(-EINVAL);
        }
        for (const auto& configured : dust::can_profile::kBuses) {
            auto& bus = buses[configured.channel];
            if (!device_is_ready(bus.dev)) {
                return fail(-ENODEV);
            }
            int result = can_set_mode(bus.dev, CAN_MODE_NORMAL);
            if (!result) {
                result = can_set_bitrate(bus.dev, configured.bitrate);
            }
            if (result) {
                return fail(result);
            }
            for (const auto& address : dust::can_profile::kReceiveFilters) {
                if (address.channel != bus.index) {
                    continue;
                }
                if (address.flags || address.id > 0x7ff) {
                    return fail(-EINVAL);
                }
                can_filter filter{};
                filter.id = address.id;
                filter.mask = 0x7ff;
                result = can_add_rx_filter(bus.dev, CanReceive, &bus, &filter);
                if (result < 0) {
                    return fail(result);
                }
            }
            result = can_start(bus.dev);
            if (result && result != -EALREADY) {
                return fail(result);
            }
            bus.ready = true;
        }
        if (dust::can_profile::kProtectOnBoot) {
            Stop(nullptr, Error::State);
            PumpCan(nullptr,
                    Now());  // 先把启动保护交给硬件，再初始化 UART / 持久计数。
        }
        for (auto& port : ports) {
            if (!device_is_ready(port.dev)) {
                return fail(-ENODEV);
            }
            if (port.index == 4) {
                uart_config config{};
                int result = uart_config_get(port.dev, &config);
                if (result) {
                    return fail(result);
                }
                config.parity = UART_CFG_PARITY_EVEN;
                config.stop_bits = UART_CFG_STOP_BITS_2;
                result = uart_configure(port.dev, &config);
                if (result) {
                    return fail(result);
                }
            }
            // HPM 原生 idle 计数上限 255 bit；921600 baud 下 200 us 为 185 bit。
            // UART 是字节流，缩短分块间隔不改变上层帧协议。
            int result = platform::StartUartReceive(port.dev, UartEvent, &port,
                                                    rx_buffers[port.index][0],
                                                    128, 200);
            if (result) {
                return fail(result);
            }
            port.ready = true;
        }
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
        imu_ready = onboard_imu.Init() == 0;
#endif
        initialized = true;
    }
    // 在首次暴露新 boot_id 前清除旧业务，持久化失败不提供可 Arm 的核心。
    if (!first) {
        Stop(nullptr, Error::State);
        PumpCan(nullptr, Now());
    }
    if (protection_errors) {
        return -EIO;
    }
    const int result = AllocateBootIdentity(boot);
    ready = !result;
    if (result) {
        Stop(nullptr, Error::Transport);
        PumpCan(nullptr, Now());
    }
    return result;
}
Protection IoProtection() {
    return {nullptr, Validate, Stop, ready};
}
void ApplyIoLimits(Limits& limits) {
    limits.can_mask = limits.uart_tx_mask = limits.uart_rx_mask = 0;
    for (const auto& bus : buses) {
        if (bus.ready) {
            limits.can_mask |= 1U << bus.index;
        }
    }
    for (const auto& port : ports) {
        if (port.ready) {
            limits.uart_rx_mask |= 1U << port.index;
            if (port.index != 4) {
                limits.uart_tx_mask |= 1U << port.index;
            }
        }
    }
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
    limits.imu = imu_ready;
#endif
}
bool CanFeedbackCapacityValid(const Configuration& value) {
    for (unsigned bus = 0; bus < 4; ++bus) {
        unsigned count = 0;
        for (const auto& filter : dust::can_profile::kReceiveFilters)
            if (filter.channel == bus) ++count;
        if (value.can_rx[bus] && value.can_rx[bus] < count) return false;
    }
    return true;
}
void ConfigureIo(const Configuration& value) {
    configuration = value;
    for (auto& bus : buses) {
        uint32_t ids[LatestCanSlots::kMaxSlots]{};
        size_t count = 0;
        if (value.can_rx[bus.index]) {
            for (const auto& filter : dust::can_profile::kReceiveFilters) {
                if (filter.channel == bus.index && count < LatestCanSlots::kMaxSlots)
                    ids[count++] = filter.id;
            }
        }
        (void)bus.latest.Configure(ids, count);
        bus.feedback_count = 0;
    }
}
void DispatchIo(Core& core, const Actions& actions, uint64_t now) {
    if (actions.status_sequence) {
        const unsigned target = actions.status_target & 3,
                       page = actions.status_target >> 2;
        const unsigned channel = actions.status_channel;
        std::array<uint32_t, 3> values{};
        int result = 0;
        if (target == 0 && !channel) {
            if (!page) {
                values = {uint32_t(core.state()),
                          dust::can_profile::kProtectionSignature,
                          dust::can_profile::kCommandTimeoutUs};
            } else if (page == 1) {
                values = {protection_errors, uint32_t(ready),
                          uint32_t(initialized)};
            } else {
                result = -ENOTSUP;
            }
        } else if (target == 1 && channel < 4) {
            const auto& bus = buses[channel];
            if (!page) {
                values = {uint32_t(atomic_get(&bus.received)),
                          uint32_t(atomic_get(&bus.submitted)),
                          uint32_t(atomic_get(&bus.completed))};
            } else if (page == 1) {
                values = {uint32_t(atomic_get(&bus.drops)),
                          uint32_t(atomic_get(&bus.errors)),
                          uint32_t(bus.transmit.size())};
            } else if (page == 2) {
                can_state state;
                can_bus_err_cnt errors{};
                result = can_get_state(bus.dev, &state, &errors);
                if (!result) {
                    values = {uint32_t(state), errors.tx_err_cnt,
                              errors.rx_err_cnt};
                }
            } else {
                result = -ENOTSUP;
            }
        } else if (target == 2 && channel >= 1 && channel <= 6) {
            const auto& port = ports[channel - 1];
            if (!page) {
                values = {uint32_t(atomic_get(&port.received)),
                          uint32_t(atomic_get(&port.drops)),
                          uint32_t(atomic_get(&port.completed))};
            } else if (page == 1) {
                values = {uint32_t(atomic_get(&port.errors)),
                          uint32_t(atomic_get(&port.busy)),
                          uint32_t(port.transmit.size())};
            } else {
                result = -ENOTSUP;
            }
        } else {
            result = -EINVAL;
        }
        core.CompleteStatus(actions.status_sequence, result, values);
    }

    if (core.state() != BusinessState::Active) {
        return;
    }
    for (const auto& bus : buses) {
        if (atomic_get(&bus.fault)) {
            core.Protect(Error::Transport);
            return;
        }
    }
    for (const auto& port : ports) {
        if (atomic_get(&port.fault)) {
            core.Protect(Error::Transport);
            return;
        }
    }
    if (capturing) {
        for (auto deadline : source_deadlines) {
            if (deadline && now >= deadline) {
                core.Protect(Error::Lease);
                return;
            }
        }
    }
    EnterActive(core, now);
    unsigned required[4]{};
    for (const auto& batch : actions.can) {
        if (batch.valid) {
            if (!batch.count || batch.count > batch.frames.size() ||
                batch.deadline_us <= now) {
                core.Protect(Error::Wire);
                return;
            }
            for (unsigned i = 0; i < batch.count; ++i) {
                if (!Validate(nullptr, 0, batch.frames[i])) {
                    core.Protect(Error::Protection);
                    return;
                }
                ++required[batch.frames[i].bus];
            }
        }
    }
    for (unsigned bus = 0; bus < 4; ++bus) {
        if (required[bus] > buses[bus].transmit.free()) {
            core.Protect(Error::Capacity);
            return;
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
                return;
            }
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        const auto& delivery = actions.uart[i];
        if (!delivery.valid) {
            continue;
        }
        if (i == 4 || !ports[i].ready || delivery.bytes.size > 512 ||
            !delivery.bytes.data) {
            core.Protect(Error::Wire);
            return;
        }
        UartWrite value;
        value.accepted = now;
        value.size = delivery.bytes.size;
        std::memcpy(value.data.data(), delivery.bytes.data, value.size);
        if (!ports[i].transmit.Push(value)) {
            continue;  // 不 ACK；主站重传，不能重复写入已接纳块。
        }
        if (!core.CommitUart(i, delivery.sequence)) {
            core.Protect(Error::Sequence);
            return;
        }
    }
}
void ServiceIo(Core& core, uint64_t now) {

    if (core.state() == BusinessState::Active) {
        EnterActive(core, now);
        for (auto deadline : source_deadlines) {
            if (deadline && now >= deadline) {
                core.Protect(Error::Lease);
                return;
            }
        }
    }
    if (core.state() == BusinessState::Ready ||
        core.state() == BusinessState::Active) {
        for (const auto& bus : buses) {
            if (atomic_get(&bus.fault)) {
                core.Protect(Error::Transport);
                return;
            }
        }
        for (const auto& port : ports) {
            if (atomic_get(&port.fault)) {
                core.Protect(Error::Transport);
                return;
            }
        }
    }
    {

        PumpCan(&core, now);  // 保护状态也需要把不可覆盖的本地保护帧交给控制器。
    }
    if (core.state() != BusinessState::Active) {
        return;
    }
    for (auto& port : ports) {
        if (atomic_get(&port.fault)) {
            core.Protect(Error::Transport);
            return;
        }
        if (atomic_get(&port.busy) || !platform::UartTransmitIdle(port.dev)) {
            if (now - port.started >= kTimeoutUs) {
                core.Protect(Error::UartTimeout);
                return;
            }
            continue;
        }
        const auto* write = port.transmit.Front();
        if (!write) {
            continue;
        }
        if (now - write->accepted >= kTimeoutUs) {
            core.Protect(Error::UartTimeout);
            return;
        }
        std::memcpy(tx_buffers[port.index], write->data.data(), write->size);
        port.started = now;
        atomic_set(&port.busy, 1);
        const int result =
            uart_tx(port.dev, tx_buffers[port.index], write->size, kTimeoutUs);
        if (result) {
            atomic_clear(&port.busy);
            core.Protect(Error::Transport);
            return;
        }
        port.transmit.Pop();
    }
}
namespace {
bool DrainCanReceive() {
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
}  // namespace
bool CanReceivePending() {
    return true; // Baseline and single-thread mode must still service queues.
}
bool ServiceCanReceive() {
    return DrainCanReceive();
}
void CaptureCanFeedback(Core::Feedback& feedback) {
    for (auto& bus : buses) {
        feedback.can[bus.index] = {bus.batch, uint32_t(atomic_get(&bus.drops)),
                                   bus.feedback.data(),
                                   uint16_t(bus.feedback_count)};
        can_state state;
        can_bus_err_cnt errors{};
        if (bus.ready && !can_get_state(bus.dev, &state, &errors)) {
            if (state == CAN_STATE_BUS_OFF) {
                atomic_set(&bus.fault, 1);
            }
        }
    }
}
void CaptureIo(Core& core, Core::Feedback& feedback, uint64_t now) {
    (void)ServiceCanReceive();
    CaptureCanFeedback(feedback);
    for (auto& port : ports) {
        if (core.state() != BusinessState::Active) {
            continue;
        }
        const unsigned capacity = configuration.uart[port.index].rx_bytes;
        if (!capacity) {
            k_msgq_purge(&port.receive);
            continue;
        }
        for (unsigned blocks = 0; blocks < 4; ++blocks) {
            if (port.offset == port.pending.size) {
                port.offset = 0;
                port.pending.size = 0;
                if (k_msgq_get(&port.receive, &port.pending, K_NO_WAIT)) {
                    break;
                }
                if (port.pending.epoch != epoch) {
                    port.pending.size = 0;
                    continue;
                }
                port.discontinuity |= port.pending.discontinuity;
            }
            // 块可能由本轮 CaptureIo 开始后的 IRQ 入队，不能用旧周期时刻计算年龄。
            now = Now();
            if (uint32_t(uint32_t(now) - port.pending.captured) >= kTimeoutUs) {
                core.Protect(Error::UartTimeout);
                return;
            }
            const size_t length =
                std::min(unsigned(port.pending.size - port.offset), capacity);
            if (!core.QueueUart(
                    port.index,
                    {port.pending.data.data() + port.offset, length},
                    port.pending.captured, now, port.discontinuity)) {
                break;
            }
            port.offset += length;
            port.discontinuity = false;
        }
    }
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
    if (imu_ready && configuration.imu) {
        modules::ImuSensor::Burst burst{};
        if (onboard_imu.TryTakeCompleted(burst)) {
            feedback.imu_valid = true;
            feedback.imu_sequence = ++imu_sequence;
            feedback.imu_capture_us = k_cyc_to_us_floor32(burst.data_ready_cycle);
            feedback.temperature = burst.temperature_c;
            std::copy(burst.rx, burst.rx + sizeof(burst.rx),
                      feedback.imu_raw.begin());
        }
        (void)onboard_imu.TryStartAsync();
    }
#endif
}
}  // namespace dust::ecat
