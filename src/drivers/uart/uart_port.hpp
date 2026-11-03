/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr UART 端口实现：异步回调只入队/记统计。 */
#pragma once

#include <array>
#include <cstddef>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "drivers/uart/uart_driver.hpp"
#include "common/types.hpp"

namespace ecat {

class UartPort {
   public:
    UartPort(const UartPortConfig* ports, size_t count);

    int Init();
    bool Ready(uint8_t port) const;
    bool TransmitIdle(uint8_t port) const;
    int Transmit(uint8_t port, const uint8_t* data, size_t length,
                 uint32_t timeout_us);
    int Abort(uint8_t port);
    bool TakeRx(uint8_t port, UartRxBlock& out);
    void SetRxEpoch(uint32_t epoch);
    UartStats Stats(uint8_t port) const;
    bool Faulted(uint8_t port) const;

   private:
    static constexpr size_t kPortCount = 6;
    static constexpr size_t kRxDepth = 4;

    struct Port {
        const device* dev = nullptr;
        uint8_t index = 0;
        bool ready = false;
        bool rx_only = false;
        alignas(4) char storage[kRxDepth * sizeof(UartRxBlock)] = {};
        k_msgq rx{};
        atomic_t busy{};
        atomic_t fault{};
        atomic_t received{};
        atomic_t drops{};
        atomic_t completed{};
        atomic_t errors{};
        atomic_t gap{};
        atomic_t cancelling{};
    };

    static void EventTrampoline(void* context, uint8_t port,
                                drivers::UartEventType type,
                                const uint8_t* data, size_t length);

    std::array<Port, kPortCount> ports_{};
    size_t port_count_ = 0;
    atomic_t epoch_{};
};

}  // namespace ecat
