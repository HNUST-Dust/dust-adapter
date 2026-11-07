/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace dust::paged {
constexpr size_t kBytes = 192, kHeader = 40, kPayload = kBytes - kHeader;
constexpr uint16_t kObject = 0x3050;
constexpr uint32_t kMagic = 0x31475044, kSignatureXor = 0x50475031;
enum class Kind : uint8_t {
    None = 0,
    Can = 1,
    Imu = 2,
    Uart = 3,
    Diagnostics = 4
};
inline uint16_t Get16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t Get32(const uint8_t* p) {
    return Get16(p) | uint32_t(Get16(p + 2)) << 16;
}
inline void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void Put32(uint8_t* p, uint32_t v) {
    Put16(p, uint16_t(v));
    Put16(p + 2, uint16_t(v >> 16));
}
inline size_t Length(Kind kind, uint8_t channel) {
    switch (kind) {
        case Kind::None:
            return channel == 0 ? 0 : SIZE_MAX;
        case Kind::Can:
            return channel == 0 || channel == 3 ? 66 : SIZE_MAX;
        case Kind::Imu:
            return channel == 0 ? 38 : SIZE_MAX;
        case Kind::Uart:
            return channel == 1 ? 143 : channel == 4 ? 47 : SIZE_MAX;
        case Kind::Diagnostics:
            return channel == 0 ? 28 : SIZE_MAX;
    }
    return SIZE_MAX;
}
inline uint32_t RecordSequence(Kind kind, const uint8_t* p) {
    return kind == Kind::None ? 0
                              : Get32(p + (kind == Kind::Diagnostics ? 8 : 0));
}
inline bool Encode(const uint8_t* common, Kind kind, uint8_t channel,
                   const uint8_t* payload, size_t length, uint8_t* out) {
    if (!common || !out || length != Length(kind, channel) ||
        length > kPayload || (!payload && length)) {
        return false;
    }
    std::fill_n(out, kBytes, uint8_t(0));
    std::copy_n(common, 22, out);
    Put32(out + 22, kMagic);
    out[26] = uint8_t(kind);
    out[27] = channel;
    Put32(out + 28, RecordSequence(kind, payload));
    Put16(out + 32, uint16_t(length));
    Put32(out + 36, Get32(common + 8));
    if (length) {
        std::copy_n(payload, length, out + kHeader);
    }
    return true;
}
struct View {
    Kind kind{};
    uint8_t channel = 0;
    const uint8_t* payload = nullptr;
    size_t length = 0;
};
inline bool Decode(const uint8_t* in, size_t bytes, View& out) {
    if (!in || bytes != kBytes || Get32(in + 22) != kMagic || Get16(in + 34) ||
        Get32(in + 36) != Get32(in + 8)) {
        return false;
    }
    const auto kind = Kind(in[26]);
    const auto length = Get16(in + 32);
    if (length != Length(kind, in[27]) || length > kPayload) {
        return false;
    }
    if (Get32(in + 28) != RecordSequence(kind, in + kHeader)) {
        return false;
    }
    if (!std::all_of(in + kHeader + length, in + kBytes,
                     [](uint8_t b) { return b == 0; })) {
        return false;
    }
    if (in[20] < 1 || in[20] > 3) {
        return false;
    }
    if ((kind == Kind::None) != (in[20] != 2)) {
        return false;
    }
    out = {kind, in[27], in + kHeader, length};
    return true;
}
/* Logical ABI5 cache. Only complete, validated pages may update one region. */
class Receiver {
   public:
    void Reset() {
        std::fill_n(image_, 410, uint8_t(0));
        boot_ = session_ = sequence_ = 0;
        have_ = false;
    }
    bool Accept(const uint8_t* page, size_t size) {
        View view;
        if (!Decode(page, size, view)) {
            return false;
        }
        const uint32_t boot = Get32(page), session = Get32(page + 4),
                       sequence = Get32(page + 8);
        if (!boot) {
            return false;
        }
        if (boot != boot_ || session != session_) {
            Reset();
            boot_ = boot;
            session_ = session;
        }
        if (have_) {
            const uint32_t delta = sequence - sequence_;
            if (!delta) {
                return std::equal(page, page + kBytes, last_page_);
            }
            if (delta >= 0x80000000) {
                return false;
            }
        }
        if (page[20] != 2) {
            std::fill_n(image_, 410, uint8_t(0));
        }
        std::copy_n(page, 22, image_);
        size_t offset = 0;
        switch (view.kind) {
            case Kind::None:
                break;
            case Kind::Can:
                offset = view.channel == 0 ? 22 : 88;
                break;
            case Kind::Imu:
                offset = 344;
                break;
            case Kind::Uart:
                offset = view.channel == 1 ? 154 : 297;
                break;
            case Kind::Diagnostics:
                offset = 382;
                break;
        }
        if (view.length) {
            std::copy_n(view.payload, view.length, image_ + offset);
        }
        std::copy_n(page, kBytes, last_page_);
        sequence_ = sequence;
        have_ = true;
        return true;
    }
    const uint8_t* data() const {
        return image_;
    }

   private:
    uint8_t image_[410]{}, last_page_[kBytes]{};
    uint32_t boot_ = 0, session_ = 0, sequence_ = 0;
    bool have_ = false;
};
}  // namespace dust::paged
