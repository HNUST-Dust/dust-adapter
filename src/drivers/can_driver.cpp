/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/can_driver.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <drivers/can/hpmicro_mcan_batch.h>

namespace dust::drivers {
namespace {

struct BusContext {
    uint8_t index = 0;
};

struct BusState {
    const device* dev = nullptr;
    bool ready = false;
    BusContext context{};
    CanTxDoneCallback done = nullptr;
    void* done_context = nullptr;
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
    std::memcpy(value.data, frame->data, frame->dlc);
    rx_callback(rx_context, state->context.index, value);
}

void TxTrampoline(const device*, int error, void* user) {
    auto* state = static_cast<BusState*>(user);
    CanTxDoneCallback done = state->done;
    state->done = nullptr;
    if (done) {
        done(state->done_context, state->context.index, error);
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
                 uint64_t deadline_us, CanTxDoneCallback done, void* context) {
    auto* state = StateFor(bus);
    if (!state || !state->ready || !state->dev) {
        return -ENODEV;
    }
    static std::array<can_frame, 16> hardware_frames;
    if (count > hardware_frames.size()) {
        return -EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        auto& target = hardware_frames[i];
        target = {};
        target.id = frames[i].id;
        target.dlc = frames[i].length;
        std::memcpy(target.data, frames[i].data, target.dlc);
    }
    state->done = done;
    state->done_context = context;
    const int result =
        deadline_us
            ? hpm_mcan_send_batch_until(state->dev, hardware_frames.data(),
                                        count, K_NO_WAIT, deadline_us,
                                        TxTrampoline, state)
            : hpm_mcan_send_batch(state->dev, hardware_frames.data(), count,
                                  K_NO_WAIT, TxTrampoline, state);
    if (result) {
        state->done = nullptr;
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

}  // namespace dust::drivers
