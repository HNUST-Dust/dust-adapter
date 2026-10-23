/* SPDX-License-Identifier: Apache-2.0 */
/** @file can_dispatch.hpp
 * @brief CAN admission, receive callbacks and transport diagnostics.
 */
#pragma once
#include "protocol.hpp"
namespace platform {
/** @brief Raw CAN record with MCU reception time and per-bus sequence. */
struct CanReceiveFrame {
    dust::wire::Record record;
    uint32_t timestamp_us, sequence;
};
using CanReceiveCallback = void (*)(const CanReceiveFrame&);
/** @brief Snapshot of CAN FIFO, controller and completion counters. */
struct CanStats {
    uint32_t received, submitted, completed, errors, dropped;
    uint32_t queue_depth, state, tx_errors, rx_errors;
};
/**
 * @brief Initialize configured buses, exact RX filters and TX workers; return
 * zero or negative errno.
 */
int InitializeCanDispatch(CanReceiveCallback callback);
/**
 * @brief Return one availability bit per initialized logical CAN channel.
 */
uint32_t CanChannelMask();
/**
 * @brief Return whether bus is in range and initialized.
 */
bool CanAvailable(uint8_t bus);
// Transmit IDs/flags and RX filters come from config/can_devices.yaml.
// Whole packet admission is checked before publishing. FIFO; no ID-based
// overwrite.
/**
 * @brief Validate the whole batch before FIFO admission; return zero or
 * negative errno.
 */
int SubmitCanRecords(const dust::wire::Record* records, size_t count);
// Discards software-queued traffic, preserving only the supplied local stop
// actions. Frames already submitted to hardware cannot be recalled by this
// interface.
/**
 * @brief Discard queued business traffic and admit protection records.
 * @note Frames already submitted to hardware cannot be recalled.
 */
int ReplaceCanTraffic(const dust::wire::Record* records, size_t count);
/**
 * @brief Return transport and controller counters for bus; invalid buses yield
 * zero counters.
 */
CanStats ReadCanStats(uint8_t bus);
/**
 * @brief Atomically read and clear the latched CAN output fault.
 */
bool TakeCanFault();
}  // namespace platform
