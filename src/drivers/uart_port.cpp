/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file uart_port.cpp
 * @brief UART 端口接收、发送队列与 USB 字节流上报。
 */
#include "drivers/uart_port.hpp"

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>

#include <algorithm>
#include <cstring>

#include "drivers/uart_receive.hpp"
#include "usb_reports.hpp"

namespace {
namespace wire = dust::wire;
constexpr int kUartTxPriority = 7;
constexpr size_t kChunkBytes = 40;
constexpr size_t kBufferBytes = 128;
constexpr int32_t kUartRxTimeoutUs = 1000;
// 所有 RX DMA 缓冲区使用统一的非缓存、缓存行对齐存储。
__nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT) uint8_t
    uart_buffers[wire::kUartPortCount][2][kBufferBytes];

/**
 * @brief UART DMA 缓冲区、流序号及原子传输计数器。
 */
struct UartPort {
    const device* dev = nullptr;
    wire::Port port{};
    unsigned buffer_set = 0;
    unsigned next_buffer = 1;
    uint32_t sequence = 0;
    atomic_t received{};
    atomic_t dropped{};
    atomic_t tx_errors{};
    atomic_t tx_completed{};
    bool available = false;

    /**
     * @brief 返回指定 DMA 缓冲区；索引必须为零或一。
     */
    uint8_t* Buffer(unsigned index) const {
        return uart_buffers[buffer_set][index];
    }

    /**
     * @brief 将 UART 字节流拆成携带会话标识的数据包，全程不阻塞。
     */
    void Publish(const uint8_t* data, size_t size) {
        while (size) {
            const size_t count = std::min(size, kChunkBytes);
            wire::UsbInPacket packet{};
            const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
            wire::UartReceiveReport report{};
            report.header.session_id = gateway::CurrentUsbReportSession();
            report.channel = uint8_t(port);
            report.byte_count = uint8_t(count);
            report.block_sequence = sequence++;
            report.receive_timestamp_us = now;
            std::memcpy(report.bytes.data(), data, count);
            wire::Encode(report, packet);
            atomic_inc(&received);
            if (gateway::CurrentUsbReportSession() &&
                !gateway::QueueSensorReportForUsb(packet)) {
                atomic_inc(&dropped);
            }
            data += count;
            size -= count;
        }
    }
};
UartPort uart_channel1{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_1)),
                       wire::Port::Channel1, 0};
UartPort uart_channel2{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_2)),
                       wire::Port::Channel2, 1};
UartPort uart_channel3{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_3)),
                       wire::Port::Channel3, 2};
UartPort uart_channel4{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_4)),
                       wire::Port::Channel4, 3};
UartPort uart_channel5{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_5)),
                       wire::Port::Channel5, 4};
UartPort uart_channel6{DEVICE_DT_GET_OR_NULL(DT_ALIAS(dust_uart_6)),
                       wire::Port::Channel6, 5};

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
UartTx transmitters[wire::kUartPortCount];
__nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT) uint8_t
    tx_buffers[wire::kUartPortCount][128];
K_MUTEX_DEFINE(tx_mutex);
K_SEM_DEFINE(tx_wake, 0, 1);
K_THREAD_STACK_DEFINE(tx_stack, 1536);
k_thread tx_thread;
atomic_t peripheral_fault;
UartPort* ports[] = {&uart_channel1, &uart_channel2, &uart_channel3,
                     &uart_channel4, &uart_channel5, &uart_channel6};

/**
 * @brief 锁存 TX 完成或错误状态，并唤醒唯一的 UART 发送工作线程。
 */
void UartTransmitDone(UartPort& port, bool error) {
    auto& tx = transmitters[unsigned(port.port) - 1];
    if (error) {
        atomic_inc(&port.tx_errors);
        atomic_set(&peripheral_fault, 1);
    } else {
        atomic_inc(&port.tx_completed);
    }
    atomic_clear(&tx.busy);
    k_sem_give(&tx_wake);
}

/**
 * @brief 在驱动回调上下文中处理异步 RX 缓冲区和 TX 完成事件。
 */
void UartEventCallback(const device* dev, uart_event* event, void* user) {
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
            // 即使没有发布数据块，也推进流序号。
            // 采集不连续后，上位机据此丢弃
            // 尚未拼接完整的帧。
            ++port.sequence;
            break;
        default:
            break;
    }
}

/**
 * @brief 发送 UART 队列中的数据；复用 DMA 存储前等待发送器空闲。
 */
void UartTxLoop(void*, void*, void*) {
    for (;;) {
        k_sem_take(&tx_wake, K_MSEC(1));
        k_mutex_lock(&tx_mutex, K_FOREVER);
        for (unsigned i = 0; i < wire::kUartPortCount; ++i) {
            auto* port = ports[i];
            if (!port || !port->available) {
                continue;
            }
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
            // HPM UART_TX_DONE 仅表示 DMA 完成，此时 FIFO
            // 和移位寄存器可能尚未排空。 uart_tx() 会重置
            // FIFO，因此必须等待发送器排空， 再启动下一次写入。
            if (!platform::UartTransmitIdle(port->dev)) {
                continue;
            }
            UartWrite write{};
            if (k_msgq_get(&tx.queue, &write, K_NO_WAIT)) {
                continue;
            }
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
 * @brief 启动固定配置的 UART 端口；返回能力位
 * CAN=1、UART10=2、UART2=4、IMU=8、UART3=16、UART11=32、UART12=64、UART15=128。
 */
uint32_t StartUartPorts() {
    uint32_t capabilities = 0;
    for (auto& tx : transmitters) {
        k_msgq_init(&tx.queue, tx.storage, sizeof(UartWrite), 8);
    }
    for (auto* port : ports) {
        if (!port) {
            continue;
        }
        if (platform::StartUartReceive(port->dev, UartEventCallback, port,
                                       port->Buffer(0), kBufferBytes,
                                       kUartRxTimeoutUs)) {
            continue;
        }
        port->available = true;
        const unsigned channel = unsigned(port->port);
        capabilities |= 1U << (channel < 3 ? channel : channel + 1);
    }
    k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
                    UartTxLoop, nullptr, nullptr, nullptr, kUartTxPriority, 0,
                    K_NO_WAIT);
    return capabilities;
}

/**
 * @brief 以原子操作读取并清除锁存的 UART 输出故障。
 */
bool TakeUartFault() {
    return atomic_cas(&peripheral_fault, 1, 0);
}

/**
 * @brief 返回指定逻辑 UART 通道是否已初始化。
 */
bool UartAvailable(uint8_t channel) {
    return channel >= 1 && channel <= wire::kUartPortCount &&
           ports[channel - 1] && ports[channel - 1]->available;
}

/**
 * @brief 将 UART 载荷复制到有界输出队列。
 * @return 接纳成功返回零；参数无效、设备不可用或队列已满时返回负 errno。
 */
int SubmitUart(uint8_t channel, const uint8_t* data, size_t length) {
    if (!UartAvailable(channel)) {
        return -ENODEV;
    }
    if (!data || !length || length > wire::kMaxUartWrite) {
        return -EINVAL;
    }
    UartWrite write{};
    write.length = length;
    std::memcpy(write.data, data, length);
    k_mutex_lock(&tx_mutex, K_FOREVER);
    const int rc =
        k_msgq_put(&transmitters[channel - 1].queue, &write, K_NO_WAIT);
    if (rc) {
        atomic_inc(&ports[channel - 1]->tx_errors);
    }
    k_mutex_unlock(&tx_mutex);
    k_sem_give(&tx_wake);
    return rc ? -ENOSPC : 0;
}

/**
 * @brief 丢弃软件队列中的 UART 写入；已启动的硬件传输无法撤回。
 */
void DiscardUartWrites() {
    k_mutex_lock(&tx_mutex, K_FOREVER);
    for (unsigned i = 0; i < wire::kUartPortCount; ++i) {
        k_msgq_purge(&transmitters[i].queue);
        if (ports[i] && atomic_get(&transmitters[i].busy)) {
            if (uart_tx_abort(ports[i]->dev)) {
                atomic_inc(&ports[i]->tx_errors);
            }
        }
    }
    k_mutex_unlock(&tx_mutex);
}

/**
 * @brief 返回通道计数器快照；未知通道返回全零计数。
 */
UartStats ReadUartStats(uint8_t channel) {
    UartStats stats{};
    if (!UartAvailable(channel)) {
        return stats;
    }
    const auto& port = *ports[channel - 1];
    stats.received = atomic_get(&port.received);
    stats.dropped = atomic_get(&port.dropped);
    stats.tx_completed = atomic_get(&port.tx_completed);
    stats.tx_errors = atomic_get(&port.tx_errors);
    return stats;
}
}  // namespace gateway
