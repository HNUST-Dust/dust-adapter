/* SPDX-License-Identifier: Apache-2.0 */
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
enum class Port : uint8_t { Uart10 = 1, Uart2 = 2, Uart3 = 3 };
enum class StopReason : uint8_t {
  None,
  Explicit,
  LeaseExpired,
  UsbLost,
  TransportError,
  Replaced
};
inline uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
inline void Put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = uint8_t(v >> (8 * i));
}
inline uint16_t U16(const uint8_t *p) {
  return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline void Put16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}
inline void Header(uint8_t *p, Kind kind, uint32_t sequence, uint32_t session) {
  Put32(p, kMagic);
  Put32(p + 4, sequence);
  p[8] = kVersion;
  p[9] = uint8_t(kind);
  Put32(p + 12, session);
}
inline bool HasHeader(const uint8_t *p, size_t size) {
  return size >= 16 && U32(p) == kMagic && p[8] == kVersion;
}
inline bool NewSequence(uint32_t next, uint32_t previous) {
  return int32_t(next - previous) > 0;
}
struct Record {
  uint8_t channel = 0, length = 0, flags = 0;
  uint32_t id = 0;
  uint8_t data[8]{};
};
inline Record Decode(const uint8_t *p) {
  Record r{};
  r.channel = p[0];
  r.length = p[1];
  r.flags = p[2];
  r.id = U32(p + 4);
  std::memcpy(r.data, p + 8, 8);
  return r;
}
inline void Encode(uint8_t *p, const Record &r) {
  p[0] = r.channel;
  p[1] = r.length;
  p[2] = r.flags;
  p[3] = 0;
  Put32(p + 4, r.id);
  std::memcpy(p + 8, r.data, 8);
}
inline bool ValidRecord(const uint8_t *p, bool allow_uart = false) {
  const Record r = Decode(p);
  if (p[3] || r.length > 8)
    return false;
  if (r.flags == kUartAction)
    return allow_uart && r.channel >= 1 && r.channel <= 3 && r.length && !r.id;
  return !(r.flags & ~(kExtended | kRemote)) && r.channel < 4 &&
         r.id <= ((r.flags & kExtended) ? 0x1fffffffU : 0x7ffU);
}
inline bool Zero(const uint8_t *p, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (p[i])
      return false;
  return true;
}
inline bool ValidRecords(const Request &p, bool safety = false) {
  if (p[10] > kMaxRecords || p[11])
    return false;
  if (!safety && !Zero(p.data() + 16, 16))
    return false;
  if (safety && !Zero(p.data() + 20, 12))
    return false;
  for (size_t i = 0; i < p[10]; ++i)
    if (!ValidRecord(p.data() + 32 + i * kRecordBytes, safety))
      return false;
  return Zero(p.data() + 32 + p[10] * kRecordBytes,
              p.size() - 32 - p[10] * kRecordBytes);
}
// Signature covers the MCU's default lease and serialized protection actions in
// file order. This is configuration identity, not cryptographic authentication
// or a CAN checksum.
inline uint32_t ProtectionSignature(uint32_t timeout_us, const Record *records,
                                    size_t count) {
  uint32_t crc = 0xffffffffU;
  const auto consume = [&crc](uint8_t byte) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
  };
  for (unsigned i = 0; i < 4; ++i)
    consume(uint8_t(timeout_us >> (8 * i)));
  for (size_t i = 0; i < count; ++i) {
    uint8_t encoded[16]{};
    Encode(encoded, records[i]);
    for (auto byte : encoded)
      consume(byte);
  }
  return crc ^ 0xffffffffU;
}
// Session policy is hardware independent so lease/sequence behavior is testable
// on a host.
class SessionPolicy {
public:
  void Open(uint32_t nonce) {
    *this = {};
    session_ = nonce;
  }
  void Observe(uint32_t seq) {
    last_sequence_ = seq;
    seen_ = true;
  }
  bool Configure(uint32_t timeout_us) {
    if (!session_ || armed_ || timeout_us < 1000 || timeout_us > 1000000)
      return false;
    timeout_ = timeout_us;
    configured_ = true;
    return true;
  }
  bool Fresh(uint32_t seq) const {
    return !seen_ || NewSequence(seq, last_sequence_);
  }
  bool Arm(uint32_t seq, uint32_t now) {
    if (!configured_ || armed_ || !Fresh(seq))
      return false;
    armed_ = true;
    last_receive_ = now;
    Observe(seq);
    return true;
  }
  bool Accept(uint32_t seq, uint32_t now) {
    if (!armed_ || !Fresh(seq) || Expired(now))
      return false;
    last_sequence_ = seq;
    last_receive_ = now;
    seen_ = true;
    return true;
  }
  bool Expired(uint32_t now) const {
    return armed_ && uint32_t(now - last_receive_) >= timeout_;
  }
  void Close() { *this = {}; }
  uint32_t Session() const { return session_; }
  bool Armed() const { return armed_; }
  bool Configured() const { return configured_; }

private:
  uint32_t session_ = 0, timeout_ = 0, last_sequence_ = 0, last_receive_ = 0;
  bool configured_ = false, armed_ = false, seen_ = false;
};
} // namespace dust::wire
