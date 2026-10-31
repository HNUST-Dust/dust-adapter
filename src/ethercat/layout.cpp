/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "protocol.hpp"
#include "pdo_schema.hpp"

namespace dust::ecat {
namespace pdo = dust::pdo;
uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t U32(const uint8_t* p) {
    return U16(p) | uint32_t(U16(p + 2)) << 16;
}
uint64_t U64(const uint8_t* p) {
    return U32(p) | uint64_t(U32(p + 4)) << 32;
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
    Put16(p, uint16_t(v));
    Put16(p + 2, uint16_t(v >> 16));
}
void Put64(uint8_t* p, uint64_t v) {
    Put32(p, uint32_t(v));
    Put32(p + 4, uint32_t(v >> 32));
}
bool Newer(uint32_t n, uint32_t old) {
    const uint32_t d = n - old;
    return d && d < 0x80000000U;
}
uint16_t ImageLayout::MappingCount() const {
    return mapping_count;
}

namespace {
class Signature {
   public:
    void Byte(uint8_t value) {
        crc_ ^= value;
        for (unsigned i = 0; i < 8; ++i) {
            crc_ = (crc_ >> 1) ^ ((crc_ & 1) ? 0xedb88320U : 0U);
        }
    }
    void Integer(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            Byte(uint8_t(value >> (8 * i)));
        }
    }
    void String(const char* text) {
        while (*text) {
            Byte(uint8_t(*text++));
        }
        Byte(0);
    }
    uint32_t Finish() const {
        return crc_ ^ 0xffffffffU;
    }

   private:
    uint32_t crc_ = 0xffffffffU;
};
class Builder {
   public:
    Builder(ImageLayout& image, uint16_t first, Signature* signature)
        : image_(image), signature_(signature), first_(first) {
        if (signature_) {
            signature_->Integer(image.bytes);
            signature_->Integer(image.MappingCount());
        } else {
            image_.bytes = 0;
            image_.count = 0;
            image_.first_pdo = first;
            image_.mapping_count = 0;
            image_.mappings = {};
        }
    }
    // Consume one (object, full name, bit length) tuple from the generated
    // field walk. The name is already fully qualified ("header.boot_id",
    // "can2.frames[0].id", ...) so no per-side prefix/naming logic remains.
    void Emit(uint16_t index, const char* name, uint8_t bits) {
        if (index != index_) {
            index_ = index;
            subindex_ = 0;
        }
        const unsigned bytes = bits / 8;
        if (error_ || count_ >= kMaxEntries || subindex_ == 255 || !bytes ||
            bytes > 30 || offset_ + bytes > kMaxImageBytes) {
            error_ = true;
            return;
        }
        const Entry entry{index_, uint8_t(++subindex_), bits,
                          uint16_t(offset_)};
        if (signature_) {
            if (count_ % 255 == 0) {
                signature_->Integer(first_ + count_ / 255);
                signature_->Integer(
                    std::min(255U, unsigned(image_.count) - count_));
            }
            signature_->Integer(entry.index);
            signature_->Integer(entry.subindex);
            signature_->Integer(entry.bits);
            signature_->Integer(entry.offset);
            signature_->String(name);
        } else {
            if (count_ % 255 == 0) {
                image_.mappings[count_ / 255] = {
                    uint16_t(first_ + count_ / 255), 0};
                image_.mapping_count++;
            }
            image_.mappings[count_ / 255].count++;
            image_.entries[count_] = entry;
            image_.count = uint16_t(count_ + 1);
            image_.bytes = uint16_t(offset_ + bytes);
        }
        count_++;
        offset_ += bytes;
    }
    uint16_t offset() const {
        return uint16_t(offset_);
    }
    bool failed() const {
        return error_;
    }

   private:
    ImageLayout& image_;
    Signature* signature_;
    uint16_t first_, index_ = 0;
    unsigned subindex_ = 0, offset_ = 0, count_ = 0;
    bool error_ = false;
};
// Record the byte offset of each region into the Layout on the first field of
// the region's object. Object index ranges mirror the schema's object bases.
void NoteRegion(Layout& l, uint16_t object, uint16_t offset) {
    if (object >= 0x2010 && object <= 0x2011) {
        l.source[object - 0x2010] = offset;
    } else if (object >= 0x3010 && object <= 0x3013) {
        l.feedback[object - 0x3010] = offset;
    } else if (object >= 0x2020 && object <= 0x2025) {
        l.uart_rx[object - 0x2020] = offset;
    } else if (object >= 0x3020 && object <= 0x3025) {
        l.uart_tx[object - 0x3020] = offset;
    } else if (object == 0x3030) {
        l.imu = offset;
    } else if (object == 0x3040) {
        l.diagnostics = offset;
    }
}
bool Image(const Configuration& c, Layout& l, bool tx, Signature* signature) {
    Builder b(tx ? l.tx : l.rx, tx ? 0x1a00 : 0x1600, signature);
    dust::pdo::Config cfg;
    cfg.can_tx = c.can_tx;
    cfg.can_rx = c.can_rx;
    for (unsigned i = 0; i < 6; ++i) {
        cfg.uart_tx[i] = c.uart[i].tx_bytes;
        cfg.uart_rx[i] = c.uart[i].rx_bytes;
    }
    cfg.imu = c.imu;
    uint16_t last_object = 0;
    const auto emit = [&](uint16_t object, const char* name, uint8_t bits) {
        if (object != last_object) {
            NoteRegion(l, object, b.offset());
            last_object = object;
        }
        b.Emit(object, name, bits);
    };
    if (tx) {
        dust::pdo::WalkTx(cfg, emit);
    } else {
        dust::pdo::WalkRx(cfg, emit);
    }
    return !b.failed();
}
bool Equal(const ImageLayout& a, const ImageLayout& b) {
    if (a.bytes != b.bytes || a.count != b.count ||
        a.first_pdo != b.first_pdo || a.count > kMaxEntries ||
        a.mapping_count != b.mapping_count ||
        a.mapping_count > a.mappings.size()) {
        return false;
    }
    for (unsigned i = 0; i < a.mapping_count; ++i) {
        if (a.mappings[i].index != b.mappings[i].index ||
            a.mappings[i].count != b.mappings[i].count) {
            return false;
        }
    }
    for (unsigned i = 0; i < a.count; ++i) {
        const auto& x = a.entries[i];
        const auto& y = b.entries[i];
        if (x.index != y.index || x.subindex != y.subindex ||
            x.bits != y.bits || x.offset != y.offset) {
            return false;
        }
    }
    return true;
}
}  // namespace
Error BuildLayout(const Configuration& c, const Limits& limits, Layout& out) {
    out = Layout{};
    if ((limits.can_mask & ~15U) ||
        ((limits.uart_tx_mask | limits.uart_rx_mask) & ~63U)) {
        return Error::Argument;
    }
    if (!ValidCycle(c.cycle_us) || c.watchdog_us != kTimeoutUs) {
        return Error::Argument;
    }
    for (auto slots : c.can_tx) {
        if (slots > 16 || (slots && !limits.can_mask)) {
            return Error::Capacity;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (c.can_rx[i] > 32 ||
            (c.can_rx[i] && !(limits.can_mask & (1U << i)))) {
            return Error::Capacity;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (c.uart[i].tx_bytes > 512 || c.uart[i].rx_bytes > 512 ||
            (c.uart[i].tx_bytes && !(limits.uart_tx_mask & (1U << i))) ||
            (c.uart[i].rx_bytes && !(limits.uart_rx_mask & (1U << i)))) {
            return Error::Capacity;
        }
    }
    if (c.imu && !limits.imu) {
        return Error::Capacity;
    }
    if (!Image(c, out, false, nullptr) || !Image(c, out, true, nullptr) ||
        out.rx.bytes > limits.rx_bytes || out.tx.bytes > limits.tx_bytes ||
        3U * (out.rx.bytes + out.tx.bytes) > limits.process_ram_bytes) {
        out = Layout{};
        return Error::Capacity;
    }
    Signature crc;
    crc.Integer(1);
    crc.Integer(kAbiMajor);
    crc.Integer(kAbiMinor);
    crc.Integer(c.cycle_us);
    Image(c, out, false, &crc);
    Image(c, out, true, &crc);
    out.signature = crc.Finish();
    return Error::None;
}
Error ConfigurationTransaction::Write(uint8_t sub, uint32_t value) {
    if (sub < 1 || sub > 22) {
        return Error::Argument;
    }
    if ((sub == 2 && !ValidCycle(value)) || (sub == 3 && value != kTimeoutUs) ||
        (sub >= 4 && sub <= 5 && value > 16) ||
        (sub >= 6 && sub <= 9 && value > 32) ||
        (sub >= 10 && sub <= 21 && value > 512) || (sub == 22 && value > 1)) {
        return Error::Argument;
    }
    if (sub == 1) {
        candidate_.signature = value;
    } else if (sub == 2) {
        candidate_.cycle_us = value;
    } else if (sub == 3) {
        candidate_.watchdog_us = value;
    } else if (sub <= 5) {
        candidate_.can_tx[sub - 4] = uint8_t(value);
    } else if (sub <= 9) {
        candidate_.can_rx[sub - 6] = uint8_t(value);
    } else if (sub <= 21) {
        auto& cap = candidate_.uart[(sub - 10) / 2];
        if ((sub - 10) % 2) {
            cap.rx_bytes = uint16_t(value);
        } else {
            cap.tx_bytes = uint16_t(value);
        }
    } else {
        candidate_.imu = value != 0;
    }
    present_ |= 1U << (sub - 1);
    return Error::None;
}
Error ConfigurationTransaction::Commit(const Limits& limits,
                                       const ImageLayout& rx,
                                       const ImageLayout& tx,
                                       Layout& layout) const {
    if (present_ != 0x3fffffU) {
        return Error::Incomplete;
    }
    // 实际映射必须独立于输出，避免重建候选布局时覆盖待校验的映射。
    if (&rx == &layout.rx || &rx == &layout.tx || &tx == &layout.rx ||
        &tx == &layout.tx) {
        return Error::Argument;
    }
    const Error error = BuildLayout(candidate_, limits, layout);
    if (error != Error::None) {
        return error;
    }
    if (candidate_.signature != layout.signature) {
        return Error::Signature;
    }
    if (!Equal(rx, layout.rx) || !Equal(tx, layout.tx)) {
        return Error::Mapping;
    }
    return Error::None;
}
void ConfigurationTransaction::Clear() {
    candidate_ = Configuration{};
    present_ = 0;
}
Error DecodeHeader(Bytes image, RxHeader& out) {
    if (!image.data || image.size < pdo::rx::header::size) return Error::Wire;
    const auto* p = image.data;
    out = {U32(p + pdo::rx::header::session_id),
           U32(p + pdo::rx::header::expected_boot_id),
           U32(p + pdo::rx::header::exchange_sequence),
           p[pdo::rx::header::control_flags],
           p[pdo::rx::header::active_source_mask],
           U16(p + pdo::rx::header::status_request_sequence),
           p[pdo::rx::header::status_target],
           p[pdo::rx::header::status_channel]};
    if (!out.session || (out.sources & ~3U) ||
        (out.flags != kDiscover && out.flags != kArm && out.flags != kStop)) {
        return Error::Wire;
    }
    return Error::None;
}
}  // namespace dust::ecat
