/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <dust_adapter/protocol.hpp>
namespace platform {
struct CanReceiveFrame {
  dust::wire::Record record;
  uint32_t timestamp_us, sequence;
};
using CanReceiveCallback = void (*)(const CanReceiveFrame &);
struct CanStats {
  uint32_t received, submitted, completed, errors, dropped;
  uint32_t queue_depth, state, tx_errors, rx_errors;
};
int InitializeCanDispatch(CanReceiveCallback callback);
uint32_t CanChannelMask();
bool CanAvailable(uint8_t bus);
// Whole packet admission is checked before publishing. FIFO; no ID-based
// overwrite.
int SubmitCanRecords(const dust::wire::Record *records, size_t count);
// Discards software-queued traffic, preserving only the supplied local stop
// actions. Frames already submitted to hardware cannot be recalled by this
// interface.
int ReplaceCanTraffic(const dust::wire::Record *records, size_t count);
CanStats ReadCanStats(uint8_t bus);
bool TakeCanFault();
} // namespace platform
