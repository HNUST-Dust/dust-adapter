/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr/MCAN 端口实现：ISR 只入队，线程侧提交批次。 */
#pragma once

#include <array>
#include <cstddef>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "drivers/can/can_driver.hpp"
#include "common/types.hpp"

namespace ecat {
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
void BeginMotorArrivalTest();
void FreezeMotorArrivalTest(uint32_t (&words)[36]);
#endif

void CopyLatestMotorRecords(uint8_t* destination, uint32_t now_us);

class CanBusPort {
   public:
    CanBusPort(const CanBusConfig* buses, size_t count);

    int Init(const CanFilterConfig* filters, size_t count);
    bool Ready(uint8_t bus) const;
    int SubmitBatch(uint8_t bus, const CanFrame* frames, size_t count,
                    uint64_t deadline_us);
    int Abort(uint8_t bus);
    int StopController(uint8_t bus);
    bool GetState(uint8_t bus, uint32_t& state, uint32_t& tx_err,
                  uint32_t& rx_err);
    bool TakeRx(uint8_t bus, CanSample& out);
    CanBusStats Stats(uint8_t bus) const;

   private:
    static constexpr size_t kBusCount = 4;
    static constexpr size_t kRxDepth = 32;

    struct Bus {
        const device* dev = nullptr;
        uint8_t index = 0;
        uint32_t bitrate = 0;
        bool ready = false;
        alignas(4) char storage[kRxDepth * sizeof(CanSample)] = {};
        k_msgq rx{};
        atomic_t busy{};
        atomic_t received{};
        atomic_t drops{};
        atomic_t completed{};
        atomic_t errors{};
    };

    static void RxTrampoline(void* context, uint8_t bus,
                             const drivers::CanFrame& frame);
    static void TxDoneTrampoline(void* context, uint8_t bus, int error);

    std::array<Bus, kBusCount> buses_{};
    size_t bus_count_ = 0;
};

}  // namespace ecat
