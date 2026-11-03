/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/uart/uart_driver.hpp"

#include <cerrno>
#include <cstring>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>

#include "drivers/uart/uart_receive.hpp"

namespace drivers {
namespace {

constexpr size_t kRxBlockSize = 128;
constexpr size_t kTxBlockSize = 512;
constexpr uint32_t kIdleTimeoutUs = 200;

struct PortState {
    const device* dev = nullptr;
    uint8_t index = 0;
    bool ready = false;
    uint8_t next_buffer = 1;
};

PortState port_states[6];
__nocache __aligned(4) uint8_t rx_buffers[6][2][kRxBlockSize];
__nocache __aligned(4) uint8_t tx_buffers[6][kTxBlockSize];
UartEventCallback event_callback = nullptr;
void* event_context = nullptr;

PortState* StateFor(uint8_t port) {
    if (port >= 6) {
        return nullptr;
    }
    return &port_states[port];
}

void UartTrampoline(const device* dev, uart_event* event, void* user) {
    auto* state = static_cast<PortState*>(user);
    if (!event_callback) {
        return;
    }
    switch (event->type) {
        case UART_TX_DONE:
            event_callback(event_context, state->index, UartEventType::TxDone,
                           nullptr, 0);
            break;
        case UART_TX_ABORTED:
            event_callback(event_context, state->index, UartEventType::TxAborted,
                           nullptr, 0);
            break;
        case UART_RX_RDY:
            event_callback(
                event_context, state->index, UartEventType::RxBlock,
                event->data.rx.buf + event->data.rx.offset, event->data.rx.len);
            break;
        case UART_RX_BUF_REQUEST: {
            const unsigned next = state->next_buffer;
            state->next_buffer ^= 1;
            if (uart_rx_buf_rsp(dev, rx_buffers[state->index][next],
                                kRxBlockSize)) {
                event_callback(event_context, state->index,
                               UartEventType::RxGap, nullptr, 0);
            }
            break;
        }
        case UART_RX_STOPPED:
            event_callback(event_context, state->index, UartEventType::RxGap,
                           nullptr, 0);
            break;
        case UART_RX_DISABLED:
            state->next_buffer = 1;
            if (uart_rx_enable(dev, rx_buffers[state->index][0], kRxBlockSize,
                               kIdleTimeoutUs)) {
                event_callback(event_context, state->index,
                               UartEventType::RxFault, nullptr, 0);
            }
            break;
        default:
            break;
    }
}

}  // namespace

int UartInit(const UartPortConfig* ports, size_t count, UartEventCallback event,
             void* context) {
    event_callback = event;
    event_context = context;
    for (size_t i = 0; i < count; ++i) {
        auto* state = StateFor(ports[i].index);
        if (!state) {
            return -EINVAL;
        }
        state->dev = ports[i].dev;
        state->index = ports[i].index;
        if (!device_is_ready(state->dev)) {
            return -ENODEV;
        }
        if (ports[i].configure_8e2) {
            uart_config config{};
            int result = uart_config_get(state->dev, &config);
            if (result) {
                return result;
            }
            config.parity = UART_CFG_PARITY_EVEN;
            config.stop_bits = UART_CFG_STOP_BITS_2;
            result = uart_configure(state->dev, &config);
            if (result) {
                return result;
            }
        }
        // HPM 原生 idle 计数上限 255 bit；921600 baud 下 200 us 为 185 bit。
        // UART 是字节流，缩短分块间隔不改变上层帧协议。
        const int result = platform::StartUartReceive(
            state->dev, UartTrampoline, state, rx_buffers[state->index][0],
            kRxBlockSize, kIdleTimeoutUs);
        if (result) {
            return result;
        }
        state->ready = true;
    }
    return 0;
}

bool UartReady(uint8_t port) {
    auto* state = StateFor(port);
    return state && state->ready;
}

bool UartTransmitIdle(uint8_t port) {
    auto* state = StateFor(port);
    return state && state->dev && platform::UartTransmitIdle(state->dev);
}

int UartTransmit(uint8_t port, const uint8_t* data, size_t length,
                 uint32_t timeout_us) {
    auto* state = StateFor(port);
    if (!state || !state->dev || !data || length > kTxBlockSize) {
        return -EINVAL;
    }
    std::memcpy(tx_buffers[state->index], data, length);
    return uart_tx(state->dev, tx_buffers[state->index], length, timeout_us);
}

int UartAbort(uint8_t port) {
    auto* state = StateFor(port);
    if (!state || !state->dev) {
        return -ENODEV;
    }
    return uart_tx_abort(state->dev);
}

}  // namespace drivers
