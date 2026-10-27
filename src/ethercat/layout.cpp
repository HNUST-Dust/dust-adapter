/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "protocol.hpp"

namespace dust::ecat {
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
    void Object(uint16_t index, const char* prefix) {
        index_ = index;
        subindex_ = 0;
        prefix_ = prefix;
    }
    void Field(const char* name, unsigned bytes) {
        if (error_ || count_ >= kMaxEntries || subindex_ == 255 || !bytes ||
            bytes > 30 || offset_ + bytes > kMaxImageBytes) {
            error_ = true;
            return;
        }
        const Entry entry{index_, uint8_t(++subindex_), uint8_t(bytes * 8),
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
            char qualified[96];
            std::snprintf(qualified, sizeof(qualified), "%s.%s", prefix_, name);
            signature_->String(qualified);
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
    void BytesField(const char* name, unsigned bytes) {
        for (unsigned n = 0; n < bytes; n += 30) {
            char chunk[64];
            std::snprintf(chunk, sizeof(chunk), "%s[%u]", name, n);
            Field(chunk, std::min(30U, bytes - n));
        }
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
    const char* prefix_ = "";
    bool error_ = false;
};
void Header(Builder& b, bool tx) {
    b.Object(tx ? 0x3000 : 0x2000, "header");
    b.Field("magic", 4);
    b.Field("abi_major", 2);
    b.Field("abi_minor", 2);
    if (tx) {
        b.Field("boot_id", 8);
        b.Field("accepted_session_id", 8);
        for (const char* name :
             {"publish_sequence", "slave_now_us", "configuration_signature",
              "capability_mask", "business_state", "fault_flags",
              "accepted_chassis_sequence", "accepted_gimbal_sequence",
              "status_reply_sequence", "reserved"}) {
            b.Field(name, 4);
        }
    } else {
        b.Field("session_id", 8);
        b.Field("expected_boot_id", 8);
        for (const char* name :
             {"exchange_sequence", "control_flags", "configuration_signature",
              "active_source_mask", "cycle_time_us",
              "status_request_sequence"}) {
            b.Field(name, 4);
        }
        b.Field("status_target", 1);
        b.Field("status_channel", 1);
        b.BytesField("reserved", 14);
    }
}
void Can(Builder& b, unsigned number, unsigned slots, bool tx) {
    char prefix[32];
    if (tx) {
        std::snprintf(prefix, sizeof(prefix), "can%u", number);
    } else {
        std::snprintf(prefix, sizeof(prefix), "can.%s",
                      number ? "gimbal" : "chassis");
    }
    b.Object(uint16_t((tx ? 0x3010 : 0x2010) + number), prefix);
    if (tx) {
        b.Field("batch_sequence", 4);
        b.Field("cumulative_drops", 4);
        b.Field("frame_count", 2);
        b.Field("flags", 2);
        b.Field("reserved", 4);
    } else {
        b.Field("source_sequence", 4);
        b.Field("remaining_valid_us", 4);
        b.Field("flags", 2);
        b.Field("frame_count", 1);
        b.Field("source_id", 1);
        b.Field("reserved", 4);
    }
    for (unsigned i = 0; i < slots; ++i) {
        auto field = [&](const char* name, unsigned size, bool bytes = false) {
            char text[64];
            std::snprintf(text, sizeof(text), "frames[%u].%s", i, name);
            if (bytes) {
                b.BytesField(text, size);
            } else {
                b.Field(text, size);
            }
        };
        if (tx) {
            field("id", 4);
            field("bus_sequence", 4);
            field("capture_time_us", 4);
            field("length", 1);
            field("flags", 1);
            field("reserved", 2);
        } else {
            field("channel", 1);
            field("length", 1);
            field("flags", 1);
            field("reserved", 1);
            field("id", 4);
        }
        field("data", 8, true);
    }
}
void Uart(Builder& b, unsigned port, unsigned bytes, bool tx) {
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "uart%u", port + 1);
    b.Object(uint16_t((tx ? 0x3020 : 0x2020) + port), prefix);
    b.Field("block_sequence", 4);
    b.Field("peer_ack_sequence", 4);
    b.Field("capture_time_us", 4);
    b.Field("length", 2);
    b.Field("flags", 2);
    b.BytesField("data", bytes);
}
void Diagnostics(Builder& b) {
    b.Object(0x3040, "diagnostics");
    for (const char* name :
         {"stop_reason", "rejected", "can_drops", "uart_drops",
          "protection_errors", "configuration_errors", "firmware_flags",
          "reserved"}) {
        b.Field(name, 4);
    }
    for (unsigned bus = 0; bus < 4; ++bus) {
        for (const char* name :
             {"received", "submitted", "completed", "dropped", "errors",
              "queue_depth", "controller_state", "packed_error_counts"}) {
            char field[64];
            std::snprintf(field, sizeof(field), "can%u.%s", bus, name);
            b.Field(field, 4);
        }
    }
    for (unsigned port = 1; port <= 6; ++port) {
        char field[32];
        std::snprintf(field, sizeof(field), "uart%u.rx_drops", port);
        b.Field(field, 4);
        std::snprintf(field, sizeof(field), "uart%u.tx_errors", port);
        b.Field(field, 4);
    }
    for (const char* name :
         {"status_result", "status_value0", "status_value1", "status_value2"}) {
        b.Field(name, 4);
    }
}
bool Image(const Configuration& c, Layout& l, bool tx, Signature* signature) {
    Builder b(tx ? l.tx : l.rx, tx ? 0x1a00 : 0x1600, signature);
    Header(b, tx);
    for (unsigned i = 0; i < (tx ? 4U : 2U); ++i) {
        const unsigned slots = tx ? c.can_rx[i] : c.can_tx[i];
        if (slots) {
            if (tx) {
                l.feedback[i] = b.offset();
            } else {
                l.source[i] = b.offset();
            }
            Can(b, i, slots, tx);
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (c.uart[i].tx_bytes || c.uart[i].rx_bytes) {
            (tx ? l.uart_tx : l.uart_rx)[i] = b.offset();
            Uart(b, i, tx ? c.uart[i].rx_bytes : c.uart[i].tx_bytes, tx);
        }
    }
    if (tx && c.imu) {
        l.imu = b.offset();
        b.Object(0x3030, "imu");
        b.Field("sample_sequence", 4);
        b.Field("capture_time_us", 4);
        b.Field("flags", 4);
        b.Field("temperature_c", 4);
        b.BytesField("spi_bytes", 25);
        b.BytesField("reserved", 7);
    }
    if (tx) {
        l.diagnostics = b.offset();
        Diagnostics(b);
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
    if (!image.data || image.size < 64 || U32(image.data) != kMagic ||
        U16(image.data + 4) != kAbiMajor || U16(image.data + 6) != kAbiMinor ||
        !ValidCycle(U32(image.data + 40))) {
        return Error::Wire;
    }
    const auto* p = image.data;
    for (unsigned i = 50; i < 64; ++i) {
        if (p[i]) {
            return Error::Wire;
        }
    }
    out = {U64(p + 8),  U64(p + 16), U32(p + 24), U32(p + 28), U32(p + 32),
           U32(p + 36), U32(p + 44), p[48],       p[49]};
    if (!out.session || (out.sources & ~3U) ||
        (out.flags != kDiscover && out.flags != kArm && out.flags != kStop)) {
        return Error::Wire;
    }
    return Error::None;
}
}  // namespace dust::ecat
