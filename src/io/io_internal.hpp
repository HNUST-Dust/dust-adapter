/* SPDX-License-Identifier: Apache-2.0 */

/* io_backend 内部共享定义：仅限 can_io/uart_io/io_backend 三个实现文件使用。 */

#ifndef ECAT_IO_INTERNAL_HPP_
#define ECAT_IO_INTERNAL_HPP_

#include <array>
#include <cstddef>
#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "io/latest_can.hpp"
#include "protocol/io_policy.hpp"
#include "protocol/protocol.hpp"

namespace dust::ecat {

inline uint64_t Now() {
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

/* can_io.cpp owns the CAN buses, uart_io.cpp owns the UART ports. */
extern CanPort buses[4];
extern UartPort ports[6];

/* UART capture epoch (owned by uart_io.cpp). */
extern atomic_t capture_epoch;
extern uint32_t epoch;

/* Shared facade state (owned by io_backend.cpp). */
extern bool initialized, ready, capturing;
extern uint32_t protection_errors;
extern Configuration configuration;

/* CAN command lease deadlines (owned by can_io.cpp). */
extern uint64_t source_deadlines[2];

/* can_io.cpp */
int CanInitialize();
void CanStop();
void CanPump(Core* core, uint64_t now);
bool CanValidate(void* user, unsigned index, const CanFrame& frame);
void CanApplyLimits(Limits& limits);
bool CanCapacityValid(const Configuration& value);
void CanConfigure(const Configuration& value);
bool CanDrainReceive();
void CanCaptureFeedback(Core::Feedback& feedback);
bool CanAnyFault();
int CanStatus(unsigned channel, unsigned page, std::array<uint32_t, 3>& values);
bool CanDispatch(Core& core, const Actions& actions, uint64_t now);

/* uart_io.cpp */
int UartInitialize();
void UartStop();
void UartApplyLimits(Limits& limits);
bool UartAnyFault();
int UartStatus(unsigned index, unsigned page, std::array<uint32_t, 3>& values);
void UartService(Core& core, uint64_t now);
void UartCapture(Core& core, Core::Feedback& feedback, uint64_t now);
bool UartDispatch(Core& core, const Actions& actions, uint64_t now);

}  // namespace dust::ecat

#endif /* ECAT_IO_INTERNAL_HPP_ */
