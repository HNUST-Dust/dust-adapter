// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "common/device_pdo.hpp"
#include "common/hi91_compact.hpp"

namespace dust::device_sensor_page {

// DTG1 activates real CAN command forwarding; the previous lab signature is rejected.
#if defined(CONFIG_ECAT_CAN_ISOLATION) || defined(DUST_CAN_ISOLATION)
constexpr uint32_t kSignature = 0x44544732;  // DTG2: wheels on CAN1/CAN2.
#else
constexpr uint32_t kSignature = 0x44544731;
#endif
constexpr size_t kBytes = device_pdo::kMotorBatchBytes;
constexpr size_t kSweepMaxBytes = 840;
constexpr size_t kSweepEntryBytes = 30;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kChunkBytes = kBytes - kHeaderBytes;
constexpr size_t kMaxRecordBytes = 128;
using Page = std::array<uint8_t, kBytes>;

enum class Kind : uint8_t { Hi91 = 0x40, Sbus = 0x41, Imu = 0x42, ImuCompact = 0x45 };
constexpr size_t kImuCompactBytes = 16;
constexpr uint8_t kLast = 1;
constexpr uint8_t kDiscontinuity = 2;
constexpr uint8_t kRetry = 4;

inline size_t Limit(Kind kind) {
    switch (kind) {
        case Kind::Hi91:
            return 128;
        case Kind::Sbus:
            return 32;
        case Kind::ImuCompact:
            return kImuCompactBytes;
        case Kind::Imu:
            return device_pdo::kImuBytes;
    }
    return 0;
}

inline void Put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = uint8_t(value >> (i * 8));
    }
}

inline uint32_t Get32(const uint8_t* p) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        result |= uint32_t(p[i]) << (i * 8);
    }
    return result;
}

inline bool Encode(Kind kind, uint32_t sequence, uint32_t capture_us,
                   bool discontinuity, const uint8_t* record, size_t size,
                   size_t offset, Page& page) {
    if (!record || size > Limit(kind) || offset >= size ||
        offset % kChunkBytes || ((kind == Kind::Imu || kind == Kind::ImuCompact) && size != Limit(kind))) {
        return false;
    }
    const size_t length = std::min(kChunkBytes, size - offset);
    page.fill(0);
    page[0] = uint8_t(kind);
    page[1] = (offset + length == size ? kLast : 0) |
              (discontinuity ? kDiscontinuity : 0);
    Put32(page.data() + 2, sequence);
    Put32(page.data() + 6, capture_us);
    page[10] = uint8_t(offset);
    page[11] = uint8_t(length);
    std::copy_n(record + offset, length, page.begin() + kHeaderBytes);
    return true;
}

struct Fragment {
    Kind kind{};
    uint32_t sequence = 0, capture_us = 0;
    uint8_t offset = 0, length = 0;
    bool last = false, discontinuity = false, retry = false;
    std::array<uint8_t, kChunkBytes> data{};
};

inline bool Decode(const uint8_t* page, size_t size, Fragment& out) {
    if (!page || size != kBytes) {
        return false;
    }
    const auto kind = Kind(page[0]);
    const auto limit = Limit(kind);
    const unsigned offset = page[10], length = page[11];
    const bool last = page[1] & kLast;
    const bool retry = page[1] & kRetry;
    if (!limit || (page[1] & ~(kLast | kDiscontinuity | kRetry)) ||
        (retry && kind != Kind::Hi91) || !length ||
        length > kChunkBytes || offset % kChunkBytes ||
        offset + length > limit ||
        (!last && (length != kChunkBytes || offset + length == limit)) ||
        ((kind == Kind::Imu || kind == Kind::ImuCompact) &&
         (!last || offset || length != limit)) ||
        !std::all_of(page + kHeaderBytes + length, page + kBytes,
                     [](uint8_t b) { return b == 0; })) {
        return false;
    }
    out = {kind,
           Get32(page + 2),
           Get32(page + 6),
           uint8_t(offset),
           uint8_t(length),
           last,
           bool(page[1] & kDiscontinuity),
           retry,
           {}};
    std::copy_n(page + kHeaderBytes, length, out.data.begin());
    return true;
}

struct Complete {
    uint32_t sequence = 0, capture_us = 0;
    size_t length = 0;
    bool discontinuity = false;
    std::array<uint8_t, kMaxRecordBytes> data{};
};

// One assembler per sensor. Session loss or replacement calls Reset.
class Assembler {
   public:
    explicit Assembler(Kind kind, uint32_t timeout_us)
        : kind_(kind), timeout_us_(timeout_us) {}

    void Reset() {
        pending_ = {};
        have_sequence_ = false;
        active_ = false;
        delivered_ = false;
        received_ = 0;
    }

    bool Accept(const uint8_t* page, size_t size, uint32_t now_us,
                Complete& out) {
        Fragment f;
        if (!Decode(page, size, f) || f.kind != kind_) {
            return false;
        }
        if (kind_ == Kind::Hi91) {
            return AcceptHi91(f, now_us, out);
        }
        if (active_ && uint32_t(now_us - started_us_) >= timeout_us_) {
            active_ = false;
        }
        if (!f.offset) {
            const uint32_t delta = f.sequence - pending_.sequence;
            const bool retry = have_sequence_ && !delta && !active_ &&
                               !delivered_ &&
                               uint32_t(now_us - started_us_) < timeout_us_;
            if (have_sequence_ && !retry && (!delta || delta >= 0x80000000U)) {
                return false;
            }
            pending_ = {};
            pending_.sequence = f.sequence;
            pending_.capture_us = f.capture_us;
            pending_.discontinuity = f.discontinuity;
            if (!retry) {
                started_us_ = now_us;
            }
            delivered_ = false;
            have_sequence_ = active_ = true;
        }
        if (!active_ || f.sequence != pending_.sequence) {
            return false;
        }
        if (f.offset < pending_.length) {
            return false;
        }
        if (f.offset != pending_.length ||
            f.capture_us != pending_.capture_us ||
            f.discontinuity != pending_.discontinuity) {
            active_ = false;
            return false;
        }
        std::copy_n(f.data.begin(), f.length, pending_.data.begin() + f.offset);
        pending_.length += f.length;
        if (!f.last) {
            return false;
        }
        out = pending_;
        delivered_ = true;
        active_ = false;
        return true;
    }

    uint8_t ReceivedMask() const { return received_; }
    uint8_t ReceivedMask(uint32_t sequence) const {
        return have_sequence_ && pending_.sequence == sequence ? received_ : 0;
    }

   private:
    bool AcceptHi91(const Fragment& f, uint32_t now_us, Complete& out) {
        const uint32_t delta = f.sequence - pending_.sequence;
        if (!have_sequence_ || (delta && delta < 0x80000000U)) {
            pending_ = {};
            pending_.sequence = f.sequence;
            pending_.capture_us = f.capture_us;
            pending_.discontinuity = f.discontinuity;
            started_us_ = now_us;
            received_ = 0;
            have_sequence_ = true;
            delivered_ = false;
        }
        if (f.sequence != pending_.sequence || delivered_ ||
            uint32_t(now_us - started_us_) >= timeout_us_ ||
            f.capture_us != pending_.capture_us ||
            f.discontinuity != pending_.discontinuity) {
            return false;
        }

        const unsigned index = f.offset / kChunkBytes;
        if (f.last) {
            pending_.length = f.offset + f.length;
        }
        std::copy_n(f.data.begin(), f.length, pending_.data.begin() + f.offset);
        received_ |= 1U << index;
        const unsigned count = (pending_.length + kChunkBytes - 1) / kChunkBytes;
        if (!count || received_ != (1U << count) - 1) {
            return false;
        }
        out = pending_;
        delivered_ = true;
        return true;
    }

    uint8_t received_ = 0;
    const Kind kind_;
    const uint32_t timeout_us_;
    Complete pending_{};
    uint32_t started_us_ = 0;
    bool have_sequence_ = false, active_ = false, delivered_ = false;
};

}  // namespace dust::device_sensor_page
