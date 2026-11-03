/* SPDX-License-Identifier: Apache-2.0 */

/* 协议 DTO：会话/通道/反馈共用的数据结构，不包含状态机实现。 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/types.hpp"

namespace ecat {

/* 上行 CAN 原始事件（由驱动在 ISR 打时间戳）。 */
using CanEvent = CanSample;

struct RxHeader {
    uint32_t session = 0;
    uint32_t boot = 0;
    uint32_t exchange = 0;
    uint32_t flags = 0;
    uint32_t sources = 0;
    uint32_t status_sequence = 0;
    uint8_t status_target = 0;
    uint8_t status_channel = 0;
};

struct CanBatch {
    bool valid = false;
    uint32_t sequence = 0;
    uint64_t deadline_us = 0;
    uint8_t count = 0;
    std::array<CanFrame, 16> frames{};
};

struct UartDelivery {
    bool valid = false;
    uint32_t sequence = 0;
    bool discontinuity = false;
    Bytes bytes{};
};

/* 输出只在本次 Process 成功后有效；UART 视图在调用方改写 Rx 影像前有效。 */
struct Actions {
    std::array<CanBatch, 2> can{};
    std::array<UartDelivery, 6> uart{};
    uint32_t status_sequence = 0;
    uint8_t status_target = 0;
    uint8_t status_channel = 0;
};

struct Protection {
    void* context = nullptr;
    /* ValidateCan 必须只验证，不产生硬件副作用。 */
    bool (*validate_can)(void*, unsigned source, const CanFrame&) = nullptr;
    void (*stop)(void*, Error) = nullptr;
    bool installed = false;
};

struct CanFeedback {
    uint32_t sequence = 0, drops = 0;
    const CanEvent* frames = nullptr;
    uint16_t count = 0;
};

struct Feedback {
    std::array<CanFeedback, 4> can{};
    bool imu_valid = false;
    uint32_t imu_sequence = 0, imu_capture_us = 0;
    float temperature = 0;
    std::array<uint8_t, 25> imu_raw{};
};

}  // namespace ecat
