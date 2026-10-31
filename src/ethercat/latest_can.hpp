/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "ethercat/protocol.hpp"

namespace dust::ecat {
// Single-owner cache. A slot is identified by its configured standard CAN ID;
// overwriting one slot does not displace other IDs or count as a transport drop.
class LatestCanSlots {
 public:
    static constexpr size_t kMaxSlots = 32;
    bool Configure(const uint32_t* ids, size_t count) {
        if (count > kMaxSlots || (count && !ids)) return false;
        for (size_t i = 0; i < count; ++i) {
            if (ids[i] > 0x7ff) return false;
            for (size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return false;
        }
        count_ = count;
        for (size_t i = 0; i < count; ++i) { ids_[i] = ids[i]; valid_[i] = false; }
        return true;
    }
    bool Update(const Core::CanEvent& frame) {
        if (frame.length > 8) return false;
        for (size_t i = 0; i < count_; ++i) {
            if (ids_[i] == frame.id) { frames_[i] = frame; valid_[i] = true; return true; }
        }
        return false;
    }
    bool Expire(uint32_t now, uint32_t max_age_us) {
        bool changed = false;
        for (size_t i = 0; i < count_; ++i) {
            if (valid_[i] && uint32_t(now - frames_[i].capture_us) >= max_age_us) {
                valid_[i] = false; changed = true;
            }
        }
        return changed;
    }
    size_t Count() const { return count_; }
    bool Valid(size_t index) const { return index < count_ && valid_[index]; }
    const Core::CanEvent& Frame(size_t index) const { return frames_[index]; }
    uint32_t Id(size_t index) const { return ids_[index]; }
 private:
    size_t count_ = 0;
    std::array<uint32_t, kMaxSlots> ids_{};
    std::array<Core::CanEvent, kMaxSlots> frames_{};
    std::array<bool, kMaxSlots> valid_{};
};
}  // namespace dust::ecat
