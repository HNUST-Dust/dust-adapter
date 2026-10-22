/* SPDX-License-Identifier: Apache-2.0 */
#include "peripherals.h"
#include <algorithm>
#include <cstring>
#include <drivers/uart_hpmicro.h>
#include <dust_adapter/onboard_imu.hpp>
#include <hpm_l1c_drv.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#ifdef CONFIG_WBR_UART10_LINE_IDLE
int Uart10LineIdleStart(const device *, uint32_t);
#endif

namespace {
namespace wire = dust::wire;
constexpr size_t kChunkBytes = 40, kBufferBytes = 128, kRingBytes = 4096;
constexpr int32_t kUartRxTimeoutUs = 1000;
__attribute__((section(".nocache"),
               aligned(64))) uint8_t uart2_ring[kRingBytes];
__attribute__((aligned(64))) uint8_t uart_buffers[2][2][kBufferBytes];
#ifdef CONFIG_WBR_UART10_LINE_IDLE
__attribute__((section(".nocache"),
               aligned(64))) uint8_t uart10_idle_buffers[2][kBufferBytes];
#endif
K_THREAD_STACK_DEFINE(uart_stack, 2048);
K_THREAD_STACK_DEFINE(imu_stack, 2048);
k_thread uart_thread, imu_thread;
modules::OnboardImu onboard;
struct UartPort {
  const device *dev = nullptr;
  wire::Port port{};
  unsigned buffer_set = 0, next_buffer = 1;
  uint32_t sequence = 0;
  atomic_t received{}, dropped{}, tx_errors{}, tx_completed{};
  bool available = false;
  uint8_t *Buffer(unsigned index) const {
#ifdef CONFIG_WBR_UART10_LINE_IDLE
    if (port == wire::Port::Uart10)
      return uart10_idle_buffers[index];
#endif
    return uart_buffers[buffer_set][index];
  }
  void Publish(const uint8_t *data, size_t size) {
    while (size) {
      const size_t count = std::min(size, kChunkBytes);
      wire::Packet packet{};
      const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
      wire::Header(packet.data(), wire::Kind::Uart, now, gateway::Session());
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
UartPort uart2{DEVICE_DT_GET(DT_NODELABEL(uart2)), wire::Port::Uart2, 0};
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart3), okay)
UartPort uart3{DEVICE_DT_GET(DT_NODELABEL(uart3)), wire::Port::Uart3, 1};
#endif

void UartTransmitDone(UartPort &, bool);

void AsyncUart(const device *dev, uart_event *event, void *user) {
  auto &port = *static_cast<UartPort *>(user);
  switch (event->type) {
  case UART_TX_DONE:
  case UART_TX_ABORTED:
    UartTransmitDone(port, event->type == UART_TX_ABORTED);
    break;
  case UART_RX_RDY: {
    const auto *data = event->data.rx.buf + event->data.rx.offset;
    const uint32_t begin =
        HPM_L1C_CACHELINE_ALIGN_DOWN(reinterpret_cast<uint32_t>(data));
    const uint32_t end = HPM_L1C_CACHELINE_ALIGN_UP(
        reinterpret_cast<uint32_t>(data) + event->data.rx.len);
    if (!(IS_ENABLED(CONFIG_WBR_UART10_LINE_IDLE) &&
          port.port == wire::Port::Uart10))
      l1c_dc_invalidate(begin, end - begin);
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
    (void)uart_rx_enable(dev, port.Buffer(0), kBufferBytes, kUartRxTimeoutUs);
    break;
  case UART_RX_STOPPED:
    // Advance stream sequence even if no chunk is emitted. Host discards a
    // partially assembled frame after an acquisition discontinuity.
    ++port.sequence;
    break;
  default:
    break;
  }
}

bool StartAsync(UartPort &port) {
  return device_is_ready(port.dev) &&
         uart_callback_set(port.dev, AsyncUart, &port) == 0 &&
         uart_rx_enable(port.dev, port.Buffer(0), kBufferBytes,
                        kUartRxTimeoutUs) == 0;
}
void Uart2Loop(void *, void *, void *) {
  size_t read = 0, observed = 0, available = 0;
  uint32_t previous_poll = k_uptime_get_32();
  for (;;) {
    size_t position = 0;
    const uint32_t now = k_uptime_get_32();
    if (now - previous_poll > 20) {
      // A whole ring can be overwritten after a prolonged scheduling
      // stall; do not forward mixed old/new bytes as a valid stream.
      ++uart2.sequence;
      if (uart_hpm_rx_circular_get_position(uart2.dev, &position) == 0)
        read = observed = position % kRingBytes;
      available = 0;
    }
    previous_poll = now;
    if (uart_hpm_rx_circular_get_position(uart2.dev, &position) == 0 &&
        position <= kRingBytes) {
      position %= kRingBytes;
      available += (position + kRingBytes - observed) % kRingBytes;
      observed = position;
      // Retain the proven DMA outstanding-write guard from production.
      size_t consume = available > 16 ? available - 16 : 0;
      while (consume) {
        const size_t count = std::min(consume, kRingBytes - read);
        uart2.Publish(uart2_ring + read, count);
        read = (read + count) % kRingBytes;
        available -= count;
        consume -= count;
      }
    }
    k_sleep(K_MSEC(1));
  }
}
void ImuLoop(void *, void *, void *) {
  uint32_t sequence = 0;
  for (;;) {
    modules::OnboardImu::Burst burst{};
    if (onboard.TryTakeCompleted(burst)) {
      wire::Packet packet{};
      const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
      wire::Header(packet.data(), wire::Kind::Imu, now, gateway::Session());
      packet[10] = modules::OnboardImu::kBurstTransferSize;
      const uint32_t age =
          k_cyc_to_us_floor32(k_cycle_get_32() - burst.data_ready_cycle);
      wire::Put32(packet.data() + 16, now - age);
      wire::Put32(packet.data() + 52, sequence++);
      std::memcpy(packet.data() + 20, burst.rx, sizeof(burst.rx));
      std::memcpy(packet.data() + 48, &burst.temperature_c, sizeof(float));
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
UartPort *ports[] = {&uart10, &uart2,
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart3), okay)
                     &uart3
#else
                     nullptr
#endif
};
void UartTransmitDone(UartPort &port, bool error) {
  auto &tx = transmitters[unsigned(port.port) - 1];
  if (error) {
    atomic_inc(&port.tx_errors);
    atomic_set(&peripheral_fault, 1);
  } else
    atomic_inc(&port.tx_completed);
  atomic_clear(&tx.busy);
  k_sem_give(&tx_wake);
}
void UartTransmitLoop(void *, void *, void *) {
  for (;;) {
    k_sem_take(&tx_wake, K_MSEC(1));
    k_mutex_lock(&tx_mutex, K_FOREVER);
    for (unsigned i = 0; i < 3; ++i) {
      auto *port = ports[i];
      if (!port || !port->available)
        continue;
      auto &tx = transmitters[i];
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
      // drain. uart_tx() resets the FIFO; never start the next write until it
      // is empty.
      if (!uart_irq_tx_complete(port->dev))
        continue;
      UartWrite write{};
      if (k_msgq_get(&tx.queue, &write, K_NO_WAIT))
        continue;
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

} // namespace

namespace gateway {
uint32_t StartPeripherals() {
  uint32_t capabilities = 0;
  for (auto &tx : transmitters)
    k_msgq_init(&tx.queue, tx.storage, sizeof(UartWrite), 8);
#ifdef CONFIG_WBR_UART10_LINE_IDLE
  if (!device_is_ready(uart10.dev) ||
      uart_hpm_rx_line_idle_enable(uart10.dev) != 0)
    return 0;
#endif
  if (StartAsync(uart10)) {
    uart10.available = true;
    capabilities |= 2;
#ifdef CONFIG_WBR_UART10_LINE_IDLE
    if (Uart10LineIdleStart(uart10.dev, kUartRxTimeoutUs) != 0)
      return 0;
#endif
  }
  if (device_is_ready(uart2.dev) &&
      uart_callback_set(uart2.dev, AsyncUart, &uart2) == 0 &&
      uart_hpm_rx_circular_enable(uart2.dev, uart2_ring, kRingBytes) == 0) {
    uart2.available = true;
    capabilities |= 4;
    k_thread_create(&uart_thread, uart_stack, K_THREAD_STACK_SIZEOF(uart_stack),
                    Uart2Loop, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
  }
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart3), okay)
  if (StartAsync(uart3)) {
    uart3.available = true;
    capabilities |= 16;
  }
#endif
  if (onboard.Init() == 0) {
    capabilities |= 8;
    k_thread_create(&imu_thread, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack),
                    ImuLoop, nullptr, nullptr, nullptr, 4, 0, K_NO_WAIT);
  }
  k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
                  UartTransmitLoop, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
  return capabilities;
}
bool TakePeripheralFault() { return atomic_cas(&peripheral_fault, 1, 0); }
bool UartAvailable(uint8_t channel) {
  return channel >= 1 && channel <= 3 && ports[channel - 1] &&
         ports[channel - 1]->available;
}
int SubmitUart(uint8_t channel, const uint8_t *data, size_t length) {
  if (!UartAvailable(channel))
    return -ENODEV;
  if (!data || !length || length > wire::kMaxUartWrite)
    return -EINVAL;
  UartWrite write{};
  write.length = length;
  std::memcpy(write.data, data, length);
  k_mutex_lock(&tx_mutex, K_FOREVER);
  const int rc =
      k_msgq_put(&transmitters[channel - 1].queue, &write, K_NO_WAIT);
  if (rc)
    atomic_inc(&ports[channel - 1]->tx_errors);
  k_mutex_unlock(&tx_mutex);
  k_sem_give(&tx_wake);
  return rc ? -ENOSPC : 0;
}
void DiscardUartWrites() {
  k_mutex_lock(&tx_mutex, K_FOREVER);
  for (unsigned i = 0; i < 3; ++i) {
    k_msgq_purge(&transmitters[i].queue);
    if (ports[i] && atomic_get(&transmitters[i].busy)) {
      if (uart_tx_abort(ports[i]->dev))
        atomic_inc(&ports[i]->tx_errors);
    }
  }
  k_mutex_unlock(&tx_mutex);
}
UartStats ReadUartStats(uint8_t channel) {
  UartStats stats{};
  if (!UartAvailable(channel))
    return stats;
  const auto &port = *ports[channel - 1];
  stats.received = atomic_get(&port.received);
  stats.dropped = atomic_get(&port.dropped);
  stats.tx_completed = atomic_get(&port.tx_completed);
  stats.tx_errors = atomic_get(&port.tx_errors);
  return stats;
}
} // namespace gateway
