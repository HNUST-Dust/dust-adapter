/* SPDX-License-Identifier: Apache-2.0 */
/** @file protocol.hpp
 * @brief Portable USB v2 encoding, validation and single-owner session policy.
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dust::wire {
constexpr uint32_t kMagic = 0x31425257;
constexpr uint8_t kVersion = 2;
constexpr size_t kInBytes = 64, kOutBytes = 128;
constexpr size_t kRecordBytes = 16,
                 kMaxRecords = (kOutBytes - 32) / kRecordBytes;
constexpr size_t kMaxUartWrite = kOutBytes - 32;
constexpr uint8_t kExtended = 1, kRemote = 2, kUartAction = 4;
constexpr uint8_t kFirmwareCanProtection = 1;
using Packet = std::array<uint8_t, kInBytes>;
using Request = std::array<uint8_t, kOutBytes>;
/** @brief USB v2 message kind identifiers. */
enum class Kind : uint8_t {
    Hello = 1,
    Ready = 2,
    Transmit = 3,
    Can = 4,
    Uart = 5,
    Imu = 6,
    Stop = 7,
    ConfigureSafety = 8,
    Arm = 9,
    Heartbeat = 10,
    Result = 11,
    Status = 12,
    UartWrite = 13
};
/** @brief Logical UART ports with fixed hardware configuration. */
enum class Port : uint8_t { Uart10 = 1, Uart2 = 2, Uart3 = 3 };
/** @brief Reason the session was invalidated. */
enum class StopReason : uint8_t {
    None,
    Explicit,
    LeaseExpired,
    UsbLost,
    TransportError,
    Replaced
};
/**
 * @brief Decode four readable bytes as an unaligned little-endian integer.
 */
inline uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}
/**
 * @brief Write v to four writable bytes in little-endian order.
 */
inline void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
/**
 * @brief Decode two readable bytes as an unaligned little-endian integer.
 */
inline uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
/**
 * @brief Write v to two writable bytes in little-endian order.
 */
inline void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
/**
 * @brief Write header fields into at least 16 writable bytes; caller zeros
 * reserved bytes.
 */
inline void Header(uint8_t* p, Kind kind, uint32_t sequence, uint32_t session) {
    Put32(p, kMagic);
    Put32(p + 4, sequence);
    p[8] = kVersion;
    p[9] = uint8_t(kind);
    Put32(p + 12, session);
}
/**
 * @brief Check minimum length, magic and version; does not validate the
 * payload.
 */
inline bool HasHeader(const uint8_t* p, size_t size) {
    return size >= 16 && U32(p) == kMagic && p[8] == kVersion;
}
/**
 * @brief Return whether next follows previous within the signed half of the
 * sequence space.
 */
inline bool NewSequence(uint32_t next, uint32_t previous) {
    return int32_t(next - previous) > 0;
}
/** @brief Decoded Classical CAN frame or optional UART stop action. */
struct Record {
    uint8_t channel = 0, length = 0, flags = 0;
    uint32_t id = 0;
    uint8_t data[8]{};
};
/**
 * @brief Decode one readable 16-byte record without validating its fields.
 */
inline Record Decode(const uint8_t* p) {
    Record r{};
    r.channel = p[0];
    r.length = p[1];
    r.flags = p[2];
    r.id = U32(p + 4);
    std::memcpy(r.data, p + 8, 8);
    return r;
}
/**
 * @brief Serialize a record into 16 writable bytes, clearing its reserved byte.
 */
inline void Encode(uint8_t* p, const Record& r) {
    p[0] = r.channel;
    p[1] = r.length;
    p[2] = r.flags;
    p[3] = 0;
    Put32(p + 4, r.id);
    std::memcpy(p + 8, r.data, 8);
}
/**
 * @brief Validate one readable 16-byte record; allow UART actions only when
 * requested.
 */
inline bool ValidRecord(const uint8_t* p, bool allow_uart = false) {
    const Record r = Decode(p);
    if (p[3] || r.length > 8) return false;
    if (r.flags == kUartAction)
        return allow_uart && r.channel >= 1 && r.channel <= 3 && r.length &&
               !r.id;
    return !(r.flags & ~(kExtended | kRemote)) && r.channel < 4 &&
           r.id <= ((r.flags & kExtended) ? 0x1fffffffU : 0x7ffU);
}
/**
 * @brief Return true if all count readable bytes are zero.
 */
inline bool Zero(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (p[i]) return false;
    return true;
}
/**
 * @brief Validate record count, reserved fields and trailing padding in a full
 * request.
 */
inline bool ValidRecords(const Request& p, bool safety = false) {
    if (p[10] > kMaxRecords || p[11]) return false;
    if (!safety && !Zero(p.data() + 16, 16)) return false;
    if (safety && !Zero(p.data() + 20, 12)) return false;
    for (size_t i = 0; i < p[10]; ++i)
        if (!ValidRecord(p.data() + 32 + i * kRecordBytes, safety))
            return false;
    return Zero(p.data() + 32 + p[10] * kRecordBytes,
                p.size() - 32 - p[10] * kRecordBytes);
}
// Signature covers the MCU's default lease and serialized protection actions in
// file order. This is configuration identity, not cryptographic authentication
// or a CAN checksum.
/**
 * @brief Compute CRC32 over the lease and ordered protection records.
 * @note Identifies configuration only; provides no authentication or
 * feedback-route identity.
 */
inline uint32_t ProtectionSignature(uint32_t timeout_us, const Record* records,
                                    size_t count) {
    uint32_t crc = 0xffffffffU;
    const auto consume = [&crc](uint8_t byte) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    };
    for (unsigned i = 0; i < 4; ++i) consume(uint8_t(timeout_us >> (8 * i)));
    for (size_t i = 0; i < count; ++i) {
        uint8_t encoded[16]{};
        Encode(encoded, records[i]);
        for (auto byte : encoded) consume(byte);
    }
    return crc ^ 0xffffffffU;
}
// Session policy is hardware independent so lease/sequence behavior is testable
// on a host.
/** @brief Hardware-independent lease and sequence state; all access requires a
 * single owner. */
class SessionPolicy {
   public:
    /**
     * @brief Reset policy and install nonce; caller must reject zero session
     * identifiers.
     */
    void Open(uint32_t nonce) {
        *this = {};
        session_ = nonce;
    }
    /**
     * @brief Record an accepted sequence without renewing the lease.
     */
    void Observe(uint32_t seq) {
        last_sequence_ = seq;
        seen_ = true;
    }
    /**
     * @brief Set a 1000..1000000 us lease before arming; return false on
     * invalid state or range.
     */
    bool Configure(uint32_t timeout_us) {
        if (!session_ || armed_ || timeout_us < 1000 || timeout_us > 1000000)
            return false;
        timeout_ = timeout_us;
        configured_ = true;
        return true;
    }
    /**
     * @brief Return whether seq is newer than the last observed sequence.
     */
    bool Fresh(uint32_t seq) const {
        return !seen_ || NewSequence(seq, last_sequence_);
    }
    /**
     * @brief Arm a configured policy with a fresh sequence and start its lease
     * at now (microseconds).
     */
    bool Arm(uint32_t seq, uint32_t now) {
        if (!configured_ || armed_ || !Fresh(seq)) return false;
        armed_ = true;
        last_receive_ = now;
        Observe(seq);
        return true;
    }
    /**
     * @brief Accept fresh traffic and renew its lease; return false if unarmed,
     * stale or expired.
     */
    bool Accept(uint32_t seq, uint32_t now) {
        if (!armed_ || !Fresh(seq) || Expired(now)) return false;
        last_sequence_ = seq;
        last_receive_ = now;
        seen_ = true;
        return true;
    }
    /**
     * @brief Check the armed lease using wrap-safe unsigned microsecond
     * subtraction.
     */
    bool Expired(uint32_t now) const {
        return armed_ && uint32_t(now - last_receive_) >= timeout_;
    }
    /**
     * @brief Reset session state and revoke forwarding permission.
     */
    void Close() { *this = {}; }
    /**
     * @brief Return the current session identifier; zero means inactive.
     */
    uint32_t Session() const { return session_; }
    /**
     * @brief Return whether forwarding permission has been granted.
     */
    bool Armed() const { return armed_; }
    /**
     * @brief Return whether a lease has been configured.
     */
    bool Configured() const { return configured_; }

   private:
    uint32_t session_ = 0, timeout_ = 0, last_sequence_ = 0, last_receive_ = 0;
    bool configured_ = false, armed_ = false, seen_ = false;
};
}  // namespace dust::wire
