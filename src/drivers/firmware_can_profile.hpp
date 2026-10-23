/* SPDX-License-Identifier: Apache-2.0 */
/** @file firmware_can_profile.hpp
 * @brief Compiled CAN allowlist and immutable protection policy checks.
 */
#pragma once
#include <can_profile_generated.hpp>

namespace dust::can_profile {
/**
 * @brief Return whether channel, identifier and flags match the compiled
 * transmit allowlist.
 */
inline bool AllowsTransmit(const wire::Record& record) {
    for (const auto& address : kTransmitAddresses)
        if (record.channel == address.channel && record.id == address.id &&
            record.flags == address.flags)
            return true;
    return false;
}
// An empty list selects MCU defaults; it never deletes firmware protection
// frames. Older v2 clients may confirm the exact default list, but cannot
// replace it.
/**
 * @brief Validate lease and protection selection; an empty CAN list retains MCU
 * defaults.
 */
inline bool AcceptsSafetyRequest(const wire::Request& request) {
    if (!wire::ValidRecords(request, true)) return false;
    const auto timeout = wire::U32(request.data() + 16);
    if (timeout < 1000 || timeout > kCommandTimeoutUs) return false;
    if (!request[10]) return true;
    size_t can_count = 0;
    for (size_t i = 0; i < request[10]; ++i) {
        const auto* record = request.data() + 32 + i * 16;
        if (record[2] == wire::kUartAction)
            continue;  // optional per-session UART actions
        if (can_count >= kProtectionFrames.size()) return false;
        uint8_t expected[16]{};
        wire::Encode(expected, kProtectionFrames[can_count++]);
        if (std::memcmp(expected, record, 16)) return false;
    }
    return !can_count || can_count == kProtectionFrames.size();
}
}  // namespace dust::can_profile
