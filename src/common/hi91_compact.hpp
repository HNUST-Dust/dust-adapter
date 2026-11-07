// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace dust::hi91 {

constexpr size_t kFrameBytes = 82;
constexpr size_t kCompactBytes = 52;
constexpr uint8_t kPageType = 0x44;
constexpr size_t kFieldsOffset = 5;
using Frame = std::array<uint8_t, kFrameBytes>;
using Compact = std::array<uint8_t, kCompactBytes>;

inline uint16_t FrameCrc(const Frame& frame) {
    uint16_t crc = 0;
    for (size_t i = 0; i < frame.size(); ++i) {
        if (i == 4 || i == 5) {
            continue;
        }
        crc ^= uint16_t(frame[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    return crc;
}

inline Frame ExpandControlFields(const Compact& compact, uint32_t sequence = 0) {
    Frame frame{0x5a, 0xa5, 76, 0, 0, 0, 0x91};
    // The host decoder's time guard uses the gateway sequence in this adapter.
    for (unsigned i = 0; i < 4; ++i) {
        frame[14 + i] = uint8_t(sequence >> (8 * i));
    }
    std::copy_n(compact.begin(), 24, frame.begin() + 18);
    std::copy_n(compact.begin() + 24, 28, frame.begin() + 54);
    const auto crc = FrameCrc(frame);
    frame[4] = uint8_t(crc);
    frame[5] = uint8_t(crc >> 8);
    return frame;
}

class Stream {
   public:
    void Discontinuity() { used_ = 0; }
    uint32_t Rejected() const { return rejected_; }
    uint32_t DeviceTimeMs() const { return device_time_ms_; }

    bool Accept(uint8_t byte, Compact& sample) {
        frame_[used_++] = byte;
        constexpr std::array<uint8_t, 4> prefix{0x5a, 0xa5, 76, 0};
        while (used_ && !std::equal(frame_.begin(),
                                    frame_.begin() + std::min(used_, prefix.size()),
                                    prefix.begin())) {
            DiscardFirst();
        }
        if (used_ != frame_.size()) {
            return false;
        }
        if (frame_[6] != 0x91 ||
            FrameCrc(frame_) != (uint16_t(frame_[4]) | (uint16_t(frame_[5]) << 8))) {
            ++rejected_;
            DiscardFirst();
            while (used_ && !std::equal(frame_.begin(),
                                        frame_.begin() + std::min(used_, prefix.size()),
                                        prefix.begin())) {
                DiscardFirst();
            }
            return false;
        }
        device_time_ms_ = 0;
        for (unsigned i = 0; i < 4; ++i) {
            device_time_ms_ |= uint32_t(frame_[14 + i]) << (8 * i);
        }
        std::copy_n(frame_.begin() + 18, 24, sample.begin());
        std::copy_n(frame_.begin() + 54, 28, sample.begin() + 24);
        used_ = 0;
        return true;
    }

   private:
    void DiscardFirst() {
        --used_;
        std::move(frame_.begin() + 1, frame_.begin() + used_ + 1, frame_.begin());
    }

    Frame frame_{};
    size_t used_ = 0;
    uint32_t rejected_ = 0, device_time_ms_ = 0;
};

}  // namespace dust::hi91
