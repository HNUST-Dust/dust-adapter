/* SPDX-License-Identifier: Apache-2.0 */
/** @file peripherals.cpp
 * @brief Fixed UART DMA streams, queued UART writes and raw IMU publication.
 */
#include "peripherals.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <algorithm>
#include <cstring>

#include "board/uart_rx_support.hpp"
#include "drivers/onboard_imu.hpp"

namespace {
namespace wire = dust::wire;
constexpr size_t kChunkBytes = 40, kBufferBytes = 128;
constexpr int32_t kUartRxTimeoutUs = 1000;
// All RX DMA buffers use the same noncached, cache-line-aligned storage.
__attribute__((section(".nocache"),
               aligned(64))) uint8_t uart_buffers[3][2][kBufferBytes];
K_THREAD_STACK_DEFINE(imu_stack, 2048);
k_thread imu_thread;
modules::OnboardImu onboard;
/** @brief UART DMA buffers, stream sequencing and atomic transport counters. */
struct UartPort {
    const device* dev = nullptr;
    wire::Port port{};
    unsigned buffer_set = 0, next_buffer = 1;
    uint32_t sequence = 0;
    atomic_t received{}, dropped{}, tx_errors{}, tx_completed{};
    bool available = false;
    /**
     * @brief Return the selected DMA buffer; index must be zero or one.
     */
    uint8_t* Buffer(unsigned index) const {
        return uart_buffers[buffer_set][index];
    }
    /**
     * @brief Split a UART byte stream into session-tagged packets without
     * blocking.
     */
    void Publish(const uint8_t* data, size_t size) {
        while (size) {
            const size_t count = std::min(size, kChunkBytes);
            wire::Packet packet{};
            const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
            wire::Header(packet.data(), wire::Kind::Uart, now,
                         gateway::Session());
            packet[10] = uint8_t(port);
            packet[11] = count;
            wire::Put32(packet.data() + 16, sequence++);
            atomic_inc(&received);
            wire::Put32(packet.data() + 20, now);
            std::memcpy(packet.data() + 24, data, count);
            if (gateway::Session() && !gateway::PublishPeripheral(packet))
                atomic_inc(&dropped);
            data += count;
            size -= count;
        }
    }
};
UartPort uart10{DEVICE_DT_GET(DT_NODELABEL(uart10)), wire::Port::Uart10, 0};
UartPort uart2{DEVICE_DT_GET(DT_NODELABEL(uart2)), wire::Port::Uart2, 1};
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart3), okay)
UartPort uart3{DEVICE_DT_GET(DT_NODELABEL(uart3)), wire::Port::Uart3, 2};
#endif

/**
 * @brief Latch TX completion or error and wake the single UART transmit worker.
 */
void UartTransmitDone(UartPort&, bool);

/**
 * @brief Handle asynchronous RX buffers and TX completion in driver callback
 * context.
 */
void AsyncUart(const device* dev, uart_event* event, void* user) {
    auto& port = *static_cast<UartPort*>(user);
    switch (event->type) {
        case UART_TX_DONE:
        case UART_TX_ABORTED:
            UartTransmitDone(port, event->type == UART_TX_ABORTED);
            break;
        case UART_RX_RDY: {
            const auto* data = event->data.rx.buf + event->data.rx.offset;
            port.Publish(data, event->data.rx.len);
            break;
        }
        case UART_RX_BUF_RELEASED:
            break;
        case UART_RX_BUF_REQUEST: {
            const unsigned next = port.next_buffer;
            port.next_buffer ^= 1;
            (void)uart_rx_buf_rsp(dev, port.Buffer(next), kBufferBytes);
            break;
        }
        case UART_RX_DISABLED:
            port.next_buffer = 1;
            (void)uart_rx_enable(dev, port.Buffer(0), kBufferBytes,
                                 kUartRxTimeoutUs);
            break;
        case UART_RX_STOPPED:
            // Advance stream sequence even if no chunk is emitted. Host
            // discards a partially assembled frame after an acquisition
            // discontinuity.
            ++port.sequence;
            break;
        default:
            break;
    }
}

/**
 * @brief Install the UART callback and start double-buffered RX; return whether
 * both succeed.
 */
bool StartAsync(UartPort& port) {
    return device_is_ready(port.dev) &&
           uart_callback_set(port.dev, AsyncUart, &port) == 0 &&
           uart_rx_enable(port.dev, port.Buffer(0), kBufferBytes,
                          kUartRxTimeoutUs) == 0;
}
/**
 * @brief Consume raw IMU bursts and publish session-tagged packets from a
 * dedicated thread.
 */
void ImuLoop(void*, void*, void*) {
    uint32_t sequence = 0;
    for (;;) {
        modules::OnboardImu::Burst burst{};
        if (onboard.TryTakeCompleted(burst)) {
            wire::Packet packet{};
            const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
            wire::Header(packet.data(), wire::Kind::Imu, now,
                         gateway::Session());
            packet[10] = modules::OnboardImu::kBurstTransferSize;
            const uint32_t age =
                k_cyc_to_us_floor32(k_cycle_get_32() - burst.data_ready_cycle);
            wire::Put32(packet.data() + 16, now - age);
            wire::Put32(packet.data() + 52, sequence++);
            std::memcpy(packet.data() + 20, burst.rx, sizeof(burst.rx));
            std::memcpy(packet.data() + 48, &burst.temperature_c,
                        sizeof(float));
            gateway::PublishPeripheral(packet);
        }
        (void)onboard.TryStartAsync();
        k_usleep(100);
    }
}
struct UartWrite {
    uint8_t length;
    uint8_t data[wire::kMaxUartWrite];
};
struct UartTx {
    k_msgq queue;
    alignas(4) char storage[8 * sizeof(UartWrite)];
    atomic_t busy{};
    uint32_t started_us = 0;
};
UartTx transmitters[3];
__attribute__((section(".nocache"), aligned(64))) uint8_t tx_buffers[3][128];
K_MUTEX_DEFINE(tx_mutex);
K_SEM_DEFINE(tx_wake, 0, 1);
K_THREAD_STACK_DEFINE(tx_stack, 1536);
k_thread tx_thread;
atomic_t peripheral_fault;
UartPort* ports[] = {&uart10, &uart2,
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart3), okay)
                     &uart3
#else
                     nullptr
#endif
};
/**
 * @brief Latch TX completion or error and wake the single UART transmit worker.
 */
void UartTransmitDone(UartPort& port, bool error) {
    auto& tx = transmitters[unsigned(port.port) - 1];
    if (error) {
        atomic_inc(&port.tx_errors);
        atomic_set(&peripheral_fault, 1);
    } else
        atomic_inc(&port.tx_completed);
    atomic_clear(&tx.busy);
    k_sem_give(&tx_wake);
}
/**
 * @brief Drain queued UART writes and wait for transmitter idle before reusing
 * DMA storage.
 */
void UartTransmitLoop(void*, void*, void*) {
    for (;;) {
        k_sem_take(&tx_wake, K_MSEC(1));
        k_mutex_lock(&tx_mutex, K_FOREVER);
        for (unsigned i = 0; i < 3; ++i) {
            auto* port = ports[i];
            if (!port || !port->available) continue;
            auto& tx = transmitters[i];
            if (atomic_get(&tx.busy)) {
                const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
                if (uint32_t(now - tx.started_us) > 100000) {
                    atomic_inc(&port->tx_errors);
                    atomic_set(&peripheral_fault, 1);
                    (void)uart_tx_abort(port->dev);
                }
                continue;
            }
            // HPM UART_TX_DONE marks DMA completion, before FIFO/shift-register
            // drain. uart_tx() resets the FIFO; never start the next write
            // until it is empty.
            if (!uart_irq_tx_complete(port->dev)) continue;
            UartWrite write{};
            if (k_msgq_get(&tx.queue, &write, K_NO_WAIT)) continue;
            std::memcpy(tx_buffers[i], write.data, write.length);
            tx.started_us = k_cyc_to_us_floor64(k_cycle_get_64());
            atomic_set(&tx.busy, 1);
            if (uart_tx(port->dev, tx_buffers[i], write.length, 100000)) {
                atomic_inc(&port->tx_errors);
                atomic_set(&peripheral_fault, 1);
                atomic_clear(&tx.busy);
            }
        }
        k_mutex_unlock(&tx_mutex);
    }
}

}  // namespace

namespace gateway {
/**
 * @brief Start fixed UART and IMU producers; return capability bits CAN=1,
 * UART10=2, UART2=4, IMU=8, UART3=16.
 */
uint32_t StartPeripherals() {
    uint32_t capabilities = 0;
    for (auto& tx : transmitters)
        k_msgq_init(&tx.queue, tx.storage, sizeof(UartWrite), 8);
    for (auto* port : ports) {
        if (!port || !device_is_ready(port->dev)) continue;
        if (board::PrepareUartReceive(port->dev)) continue;
        if (!StartAsync(*port)) continue;
        if (board::StartUartReceive(port->dev, kUartRxTimeoutUs)) {
            (void)uart_rx_disable(port->dev);
            continue;
        }
        port->available = true;
        capabilities |= port->port == wire::Port::Uart3
                            ? 16U
                            : (1U << unsigned(port->port));
    }
    if (onboard.Init() == 0) {
        capabilities |= 8;
        k_thread_create(&imu_thread, imu_stack,
                        K_THREAD_STACK_SIZEOF(imu_stack), ImuLoop, nullptr,
                        nullptr, nullptr, 4, 0, K_NO_WAIT);
    }
    k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
                    UartTransmitLoop, nullptr, nullptr, nullptr, 7, 0,
                    K_NO_WAIT);
    return capabilities;
}
/**
 * @brief Atomically consume the latched UART output fault.
 */
bool TakePeripheralFault() { return atomic_cas(&peripheral_fault, 1, 0); }
/**
 * @brief Return whether the requested logical UART channel is initialized.
 */
bool UartAvailable(uint8_t channel) {
    return channel >= 1 && channel <= 3 && ports[channel - 1] &&
           ports[channel - 1]->available;
}
/**
 * @brief Copy a UART payload into its bounded output queue.
 * @return Zero on admission, or negative errno for invalid, unavailable or full
 * output.
 */
int SubmitUart(uint8_t channel, const uint8_t* data, size_t length) {
    if (!UartAvailable(channel)) return -ENODEV;
    if (!data || !length || length > wire::kMaxUartWrite) return -EINVAL;
    UartWrite write{};
    write.length = length;
    std::memcpy(write.data, data, length);
    k_mutex_lock(&tx_mutex, K_FOREVER);
    const int rc =
        k_msgq_put(&transmitters[channel - 1].queue, &write, K_NO_WAIT);
    if (rc) atomic_inc(&ports[channel - 1]->tx_errors);
    k_mutex_unlock(&tx_mutex);
    k_sem_give(&tx_wake);
    return rc ? -ENOSPC : 0;
}
/**
 * @brief Discard software-queued UART writes; already active hardware transfers
 * cannot be recalled.
 */
void DiscardUartWrites() {
    k_mutex_lock(&tx_mutex, K_FOREVER);
    for (unsigned i = 0; i < 3; ++i) {
        k_msgq_purge(&transmitters[i].queue);
        if (ports[i] && atomic_get(&transmitters[i].busy)) {
            if (uart_tx_abort(ports[i]->dev)) atomic_inc(&ports[i]->tx_errors);
        }
    }
    k_mutex_unlock(&tx_mutex);
}
/**
 * @brief Return a snapshot of per-channel counters; unknown channels yield zero
 * counters.
 */
UartStats ReadUartStats(uint8_t channel) {
    UartStats stats{};
    if (!UartAvailable(channel)) return stats;
    const auto& port = *ports[channel - 1];
    stats.received = atomic_get(&port.received);
    stats.dropped = atomic_get(&port.dropped);
    stats.tx_completed = atomic_get(&port.tx_completed);
    stats.tx_errors = atomic_get(&port.tx_errors);
    return stats;
}
}  // namespace gateway
