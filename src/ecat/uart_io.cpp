/* SPDX-License-Identifier: Apache-2.0 */

/* UART 后端：中断/DMA 接收队列、异步发送、窗口状态与跨周期快照。 */

#include "ecat/io_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace dust::ecat {

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

namespace {
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
}  // namespace

int UartInitialize() {
    for (auto& port : ports) {
        if (!device_is_ready(port.dev)) {
            return -ENODEV;
        }
        if (port.index == 4) {
            uart_config config{};
            int result = uart_config_get(port.dev, &config);
            if (result) {
                return result;
            }
            config.parity = UART_CFG_PARITY_EVEN;
            config.stop_bits = UART_CFG_STOP_BITS_2;
            result = uart_configure(port.dev, &config);
            if (result) {
                return result;
            }
        }
        // HPM 原生 idle 计数上限 255 bit；921600 baud 下 200 us 为 185 bit。
        // UART 是字节流，缩短分块间隔不改变上层帧协议。
        int result = platform::StartUartReceive(port.dev, UartEvent, &port,
                                                rx_buffers[port.index][0], 128,
                                                200);
        if (result) {
            return result;
        }
        port.ready = true;
    }
    return 0;
}

void UartStop() {
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
}

void UartApplyLimits(Limits& limits) {
    for (const auto& port : ports) {
        if (port.ready) {
            limits.uart_rx_mask |= 1U << port.index;
            if (port.index != 4) {
                limits.uart_tx_mask |= 1U << port.index;
            }
        }
    }
}

bool UartAnyFault() {
    for (const auto& port : ports) {
        if (atomic_get(&port.fault)) {
            return true;
        }
    }
    return false;
}

int UartStatus(unsigned index, unsigned page,
               std::array<uint32_t, 3>& values) {
    const auto& port = ports[index];
    if (!page) {
        values = {uint32_t(atomic_get(&port.received)),
                  uint32_t(atomic_get(&port.drops)),
                  uint32_t(atomic_get(&port.completed))};
        return 0;
    }
    if (page == 1) {
        values = {uint32_t(atomic_get(&port.errors)),
                  uint32_t(atomic_get(&port.busy)),
                  uint32_t(port.transmit.size())};
        return 0;
    }
    return -ENOTSUP;
}

void UartService(Core& core, uint64_t now) {
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

void UartCapture(Core& core, Core::Feedback& feedback, uint64_t now) {
    (void)feedback;
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
}

bool UartDispatch(Core& core, const Actions& actions, uint64_t now) {
    for (unsigned i = 0; i < 6; ++i) {
        const auto& delivery = actions.uart[i];
        if (!delivery.valid) {
            continue;
        }
        if (i == 4 || !ports[i].ready || delivery.bytes.size > 512 ||
            !delivery.bytes.data) {
            core.Protect(Error::Wire);
            return false;
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
            return false;
        }
    }
    return true;
}

}  // namespace dust::ecat
