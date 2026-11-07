/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/can/can_driver.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <drivers/can/hpmicro_mcan_batch.h>
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
#include <drivers/can/hpmicro_mcan_timing.h>
#endif

namespace drivers {
namespace {

struct BusContext {
    uint8_t index = 0;
};

struct BusState {
    const device* dev = nullptr;
    bool ready = false;
    bool tx_pending = false;
    BusContext context{};
    CanTxDoneCallback done = nullptr;
    void* done_context = nullptr;
    std::array<can_frame, 16> hardware_frames{};
};

BusState bus_states[4];
CanRxCallback rx_callback = nullptr;
void* rx_context = nullptr;

void RxTrampoline(const device*, can_frame* frame, void* user) {
    auto* state = static_cast<BusState*>(user);
    if (!rx_callback) {
        return;
    }
    CanFrame value;
    value.id = frame->id;
    value.length = frame->dlc;
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
    if (const auto* timing = hpm_mcan_last_rx_timing(state->dev)) {
        value.arrival_timestamp = uint16_t(timing->rx_timestamp);
        value.arrival_age_ticks = uint16_t(timing->counter - timing->rx_timestamp);
    }
#endif
    std::memcpy(value.data, frame->data, frame->dlc);
    rx_callback(rx_context, state->context.index, value);
}

void TxTrampoline(const device*, int error, void* user) {
    auto* state = static_cast<BusState*>(user);
    const auto key = irq_lock();
    CanTxDoneCallback done = state->done;
    void* context = state->done_context;
    state->done = nullptr;
    state->done_context = nullptr;
    state->tx_pending = false;
    irq_unlock(key);
    if (done) {
        done(context, state->context.index, error);
    }
}

BusState* StateFor(uint8_t bus) {
    if (bus >= 4) {
        return nullptr;
    }
    return &bus_states[bus];
}

}  // namespace

int CanInit(const CanBusConfig* buses, size_t bus_count,
            const CanFilterConfig* filters, size_t filter_count,
            CanRxCallback rx, void* context) {
    rx_callback = rx;
    rx_context = context;
    for (size_t i = 0; i < bus_count; ++i) {
        auto* state = StateFor(buses[i].index);
        if (!state) {
            return -EINVAL;
        }
        state->dev = buses[i].dev;
        state->context.index = buses[i].index;
        if (!device_is_ready(state->dev)) {
            return -ENODEV;
        }
        int result = can_set_mode(state->dev, CAN_MODE_NORMAL);
        if (!result) {
            result = can_set_bitrate(state->dev, buses[i].bitrate);
        }
        if (result) {
            return result;
        }
        for (size_t f = 0; f < filter_count; ++f) {
            if (filters[f].bus != buses[i].index) {
                continue;
            }
            can_filter filter{};
            filter.id = filters[f].id;
            filter.mask = filters[f].mask;
            result = can_add_rx_filter(state->dev, RxTrampoline, state, &filter);
            if (result < 0) {
                return result;
            }
        }
        result = can_start(state->dev);
        if (result && result != -EALREADY) {
            return result;
        }
        state->ready = true;
    }
    return 0;
}

bool CanReady(uint8_t bus) {
    auto* state = StateFor(bus);
    return state && state->ready;
}

int CanSendBatch(uint8_t bus, const CanFrame* frames, size_t count,
                 uint64_t deadline_us, CanTxDoneCallback done, void* context,
                 bool (*admit)(void*), void* admit_context) {
    auto* state = StateFor(bus);
    if (!state || !state->ready || !state->dev) {
        return -ENODEV;
    }
    auto& hardware_frames = state->hardware_frames;
    if (!frames || !count || count > hardware_frames.size()) {
        return -EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (frames[i].id > 0x7ff || frames[i].length > 8) {
            return -EINVAL;
        }
    }
    const auto key = irq_lock();
    if (state->tx_pending) {
        irq_unlock(key);
        return -EAGAIN;
    }
    state->tx_pending = true;
    state->done = done;
    state->done_context = context;
    irq_unlock(key);
    for (size_t i = 0; i < count; ++i) {
        auto& target = hardware_frames[i];
        target = {};
        target.id = frames[i].id;
        target.dlc = frames[i].length;
        std::memcpy(target.data, frames[i].data, target.dlc);
    }
    const int result = admit
        ? hpm_mcan_send_batch_guarded(state->dev, hardware_frames.data(), count,
                                     K_NO_WAIT, deadline_us, TxTrampoline, state,
                                     admit, admit_context)
        : deadline_us
            ? hpm_mcan_send_batch_until(state->dev, hardware_frames.data(),
                                        count, K_NO_WAIT, deadline_us,
                                        TxTrampoline, state)
            : hpm_mcan_send_batch(state->dev, hardware_frames.data(), count,
                                  K_NO_WAIT, TxTrampoline, state);
    if (result) {
        const auto failed_key = irq_lock();
        state->done = nullptr;
        state->done_context = nullptr;
        state->tx_pending = false;
        irq_unlock(failed_key);
    }
    return result;
}



int CanAbortTx(uint8_t bus) {
    auto* state = StateFor(bus);
    if (!state || !state->dev) {
        return -ENODEV;
    }
    return hpm_mcan_abort_tx(state->dev, 2000);
}

void CanStopController(uint8_t bus) {
    auto* state = StateFor(bus);
    if (!state || !state->dev) {
        return;
    }
    state->ready = false;
    (void)can_stop(state->dev);
}

bool CanGetState(uint8_t bus, can_state& state_out, can_bus_err_cnt& errors) {
    auto* state = StateFor(bus);
    if (!state || !state->dev) {
        return false;
    }
    return can_get_state(state->dev, &state_out, &errors) == 0;
}

}  // namespace drivers
