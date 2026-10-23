/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <limits>

#include "usb_protocol.hpp"
namespace dust::wire {
// 返回实际 USB 上行长度；零表示消息类型或 CAN 帧数无效。
inline size_t UsbReportLength(const UsbInPacket& packet) {
    switch (Kind(packet[9])) {
        case Kind::CanReceiveReport:
            return packet[10] >= 1 && packet[10] <= kMaxCanFramesPerPacket
                       ? 16 + packet[10] * kCanReceiveRecordBytes
                       : 0;
        case Kind::Ready:
        case Kind::UartReceiveReport:
        case Kind::ImuSampleReport:
        case Kind::Result:
        case Kind::Status:
            return kStandardInBytes;
        default:
            return 0;
    }
}

/**
 * @brief 将四个可读字节解码为小端整数，无需地址对齐。
 */
inline uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

/**
 * @brief 将 v 按小端顺序写入四个可写字节。
 */
inline void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = uint8_t(v >> (8 * i));
    }
}

/**
 * @brief 将两个可读字节解码为小端整数，无需地址对齐。
 */
inline uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

/**
 * @brief 将 v 按小端顺序写入两个可写字节。
 */
inline void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

/**
 * @brief 将包头写入至少 16 字节的可写区域；保留字节由调用者清零。
 */
inline void Header(uint8_t* p, Kind kind, uint32_t sequence, uint32_t session) {
    Put32(p, kMagic);
    Put32(p + 4, sequence);
    p[8] = kVersion;
    p[9] = uint8_t(kind);
    Put32(p + 12, session);
}

/**
 * @brief 检查最小长度、魔数和版本，不校验载荷。
 */
inline bool HasHeader(const uint8_t* p, size_t size) {
    return size >= 16 && U32(p) == kMagic && p[8] == kVersion;
}

// 字段访问不校验请求；接纳操作前须校验包头，
// 并按消息类型校验载荷。
class UsbOutRequestView {
   public:
    explicit UsbOutRequestView(const UsbOutRequest& request)
        : request_(request) {}

    Kind MessageKind() const {
        return Kind(request_[9]);
    }
    uint32_t Sequence() const {
        return U32(request_.data() + 4);
    }
    uint32_t Session() const {
        return U32(request_.data() + 12);
    }
    uint32_t LeaseUs() const {
        return U32(request_.data() + 16);
    }
    uint8_t RecordCount() const {
        return request_[10];
    }
    uint8_t UartChannel() const {
        return request_[10];
    }
    uint8_t UartLength() const {
        return request_[11];
    }
    uint8_t StatusTarget() const {
        return request_[10];
    }
    uint8_t StatusChannel() const {
        return request_[11];
    }
    const uint8_t* Payload() const {
        return request_.data() + 32;
    }

   private:
    const UsbOutRequest& request_;
};

/**
 * @brief 在序号空间的有符号半区间内，判断 next 是否晚于 previous。
 */
inline bool NewSequence(uint32_t next, uint32_t previous) {
    return int32_t(next - previous) > 0;
}

/**
 * @brief 解码一条 16 字节可读记录，不校验字段。
 */
inline CanFrame Decode(const uint8_t* p) {
    CanFrame r{};
    r.channel = p[0];
    r.length = p[1];
    r.flags = p[2];
    r.id = U32(p + 4);
    std::memcpy(r.data, p + 8, 8);
    return r;
}

/**
 * @brief 将记录序列化到 16 字节可写区域，并清零保留字节。
 */
inline void Encode(uint8_t* p, const CanFrame& r) {
    p[0] = r.channel;
    p[1] = r.length;
    p[2] = r.flags;
    p[3] = 0;
    Put32(p + 4, r.id);
    std::memcpy(p + 8, r.data, 8);
}

/**
 * @brief 校验一条 16 字节可读记录；仅在显式允许时接纳 UART 动作。
 */
inline bool ValidRecord(const uint8_t* p, bool allow_uart = false) {
    const CanFrame r = Decode(p);
    if (p[3] || r.length > 8) {
        return false;
    }
    if (r.flags == kUartAction) {
        return allow_uart && r.channel >= 1 && r.channel <= kUartPortCount &&
               r.length && !r.id;
    }
    return !(r.flags & ~(kExtended | kRemote)) && r.channel < 4 &&
           r.id <= ((r.flags & kExtended) ? 0x1fffffffU : 0x7ffU);
}

/**
 * @brief 当 count 个可读字节全部为零时返回 true。
 */
inline bool Zero(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 校验完整请求中的记录数量、保留字段和尾部填充。
 */
inline bool ValidRecords(const UsbOutRequest& p, bool safety = false) {
    if (p[10] > kMaxRecords || p[11]) {
        return false;
    }
    if (!safety && !Zero(p.data() + 16, 16)) {
        return false;
    }
    if (safety && !Zero(p.data() + 20, 12)) {
        return false;
    }
    for (size_t i = 0; i < p[10]; ++i) {
        if (!ValidRecord(p.data() + 32 + i * kRecordBytes, safety)) {
            return false;
        }
    }
    return Zero(p.data() + 32 + p[10] * kRecordBytes,
                p.size() - 32 - p[10] * kRecordBytes);
}
/** @brief 主机请求向 CAN 总线发送的帧批次；读取前须通过 IsValid()。 */
class CanTransmitRequestView {
   public:
    explicit CanTransmitRequestView(const UsbOutRequest& packet)
        : packet_(packet) {}
    bool IsValid() const {
        return Kind(packet_[9]) == Kind::CanTransmitRequest &&
               ValidRecords(packet_);
    }
    size_t FrameCount() const {
        return packet_[10];
    }
    CanFrame Frame(size_t index) const {
        return Decode(packet_.data() + 32 + index * kRecordBytes);
    }

   private:
    const UsbOutRequest& packet_;
};

/** @brief 主机请求向 UART 发送的字节流；读取前须通过 IsValid()。 */
class UartTransmitRequestView {
   public:
    explicit UartTransmitRequestView(const UsbOutRequest& packet)
        : packet_(packet) {}
    bool IsValid() const {
        return Kind(packet_[9]) == Kind::UartTransmitRequest &&
               Channel() >= 1 && Channel() <= kUartPortCount && ByteCount() &&
               ByteCount() <= kMaxUartWrite && Zero(packet_.data() + 16, 16) &&
               Zero(Bytes() + ByteCount(), packet_.size() - 32 - ByteCount());
    }
    uint8_t Channel() const {
        return packet_[10];
    }
    size_t ByteCount() const {
        return packet_[11];
    }
    const uint8_t* Bytes() const {
        return packet_.data() + 32;
    }

   private:
    const UsbOutRequest& packet_;
};

/** @brief 将 MCU 从 CAN 总线收到的帧打包上报，每包最多六帧。 */
class CanReceiveReportBuilder {
   public:
    CanReceiveReportBuilder(UsbInPacket& packet, uint32_t session)
        : packet_(packet) {
        packet_ = {};
        Header(packet_.data(), Kind::CanReceiveReport, 0, session);
    }
    bool AddFrame(const CanFrame& frame, uint32_t received_us,
                  uint32_t bus_sequence) {
        if (FrameCount() >= kMaxCanFramesPerPacket) {
            return false;
        }
        auto* record = packet_.data() + 16 + FrameCount() * 24;
        Encode(record, frame);
        Put32(record + 16, received_us);
        Put32(record + 20, bus_sequence);
        ++packet_[10];
        return true;
    }
    size_t FrameCount() const {
        return packet_[10];
    }

   private:
    UsbInPacket& packet_;
};

/** @brief 构造 UART 接收字节块上报；块边界不等于外设协议帧边界。 */
inline bool BuildUartReceiveReport(UsbInPacket& packet, uint32_t session,
                                   uint8_t channel, uint32_t block_sequence,
                                   uint32_t received_us, const uint8_t* bytes,
                                   size_t count) {
    if (!bytes || !count || count > 40 || channel < 1 ||
        channel > kUartPortCount) {
        return false;
    }
    packet = {};
    Header(packet.data(), Kind::UartReceiveReport, 0, session);
    packet[10] = channel;
    packet[11] = uint8_t(count);
    Put32(packet.data() + 16, block_sequence);
    Put32(packet.data() + 20, received_us);
    std::memcpy(packet.data() + 24, bytes, count);
    return true;
}

// 签名覆盖 MCU 默认租约及按文件顺序序列化的保护动作。
// 它用于标识配置，不提供密码学认证，
// 也不是 CAN 校验和。

/**
 * @brief 计算租约及有序保护记录的 CRC32。
 * @note 仅标识配置，不提供认证，也不标识反馈路由。
 */
inline uint32_t ProtectionSignature(uint32_t timeout_us,
                                    const CanFrame* records, size_t count) {
    uint32_t crc = 0xffffffffU;
    const auto consume = [&crc](uint8_t byte) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    };
    for (unsigned i = 0; i < 4; ++i) {
        consume(uint8_t(timeout_us >> (8 * i)));
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t encoded[16]{};
        Encode(encoded, records[i]);
        for (auto byte : encoded) {
            consume(byte);
        }
    }
    return crc ^ 0xffffffffU;
}

inline bool Encode(const CanTransmitRequest& m, UsbOutRequest& p) {
    if (m.frame_count > kMaxCanFramesPerPacket) {
        return false;
    }
    p = {};
    Header(p.data(), Kind::CanTransmitRequest, m.header.request_sequence,
           m.header.session_id);
    p[10] = m.frame_count;
    for (size_t i = 0; i < m.frame_count; ++i) {
        Encode(p.data() + 32 + i * kRecordBytes, m.frames[i]);
    }
    return ValidRecords(p);
}
inline bool Decode(const UsbOutRequest& p, CanTransmitRequest& m) {
    if (!HasHeader(p.data(), p.size()) ||
        !CanTransmitRequestView(p).IsValid()) {
        return false;
    }
    CanTransmitRequest result{};
    result.header = {U32(p.data() + 4), U32(p.data() + 12)};
    result.frame_count = p[10];
    for (size_t i = 0; i < result.frame_count; ++i) {
        result.frames[i] = Decode(p.data() + 32 + i * kRecordBytes);
    }
    m = result;
    return true;
}
inline bool Encode(const CanReceiveReport& m, UsbInPacket& p) {
    if (!m.frame_count || m.frame_count > kMaxCanFramesPerPacket) {
        return false;
    }
    CanReceiveReportBuilder builder(p, m.header.session_id);
    Put32(p.data() + 4, m.header.transmit_timestamp_us);
    for (size_t i = 0; i < m.frame_count; ++i) {
        const auto& f = m.frames[i];
        builder.AddFrame(f.frame, f.receive_timestamp_us, f.bus_sequence);
    }
    return true;
}
inline bool Encode(const UartTransmitRequest& m, UsbOutRequest& p) {
    if (!m.byte_count || m.byte_count > kMaxUartWrite || m.channel < 1 ||
        m.channel > kUartPortCount) {
        return false;
    }
    p = {};
    Header(p.data(), Kind::UartTransmitRequest, m.header.request_sequence,
           m.header.session_id);
    p[10] = m.channel;
    p[11] = m.byte_count;
    std::memcpy(p.data() + 32, m.bytes.data(), m.byte_count);
    return true;
}
inline bool Decode(const UsbOutRequest& p, UartTransmitRequest& m) {
    const UartTransmitRequestView view(p);
    if (!HasHeader(p.data(), p.size()) || !view.IsValid()) {
        return false;
    }
    UartTransmitRequest result{};
    result.header = {U32(p.data() + 4), U32(p.data() + 12)};
    result.channel = view.Channel();
    result.byte_count = uint8_t(view.ByteCount());
    std::memcpy(result.bytes.data(), view.Bytes(), result.byte_count);
    m = result;
    return true;
}
inline bool Encode(const UartReceiveReport& m, UsbInPacket& p) {
    if (!BuildUartReceiveReport(p, m.header.session_id, m.channel,
                                m.block_sequence, m.receive_timestamp_us,
                                m.bytes.data(), m.byte_count)) {
        return false;
    }
    Put32(p.data() + 4, m.header.transmit_timestamp_us);
    return true;
}

inline bool Encode(const HelloRequest& m, UsbOutRequest& p) {
    p = {};
    Header(p.data(), Kind::Hello, m.header.request_sequence,
           m.header.session_id);
    return true;
}
inline bool Decode(const UsbOutRequest& p, HelloRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::Hello || p[10] ||
        p[11] || !Zero(p.data() + 16, 112)) {
        return false;
    }
    m.header = {U32(p.data() + 4), U32(p.data() + 12)};
    return true;
}
inline bool Encode(const ArmRequest& m, UsbOutRequest& p) {
    p = {};
    Header(p.data(), Kind::Arm, m.header.request_sequence, m.header.session_id);
    return true;
}
inline bool Decode(const UsbOutRequest& p, ArmRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::Arm || p[10] ||
        p[11] || !Zero(p.data() + 16, 112)) {
        return false;
    }
    m.header = {U32(p.data() + 4), U32(p.data() + 12)};
    return true;
}
inline bool Encode(const StopRequest& m, UsbOutRequest& p) {
    p = {};
    Header(p.data(), Kind::Stop, m.header.request_sequence,
           m.header.session_id);
    return true;
}
inline bool Decode(const UsbOutRequest& p, StopRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::Stop || p[10] ||
        p[11] || !Zero(p.data() + 16, 112)) {
        return false;
    }
    m.header = {U32(p.data() + 4), U32(p.data() + 12)};
    return true;
}
inline bool Encode(const HeartbeatRequest& m, UsbOutRequest& p) {
    p = {};
    Header(p.data(), Kind::Heartbeat, m.header.request_sequence,
           m.header.session_id);
    return true;
}
inline bool Decode(const UsbOutRequest& p, HeartbeatRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::Heartbeat ||
        p[10] || p[11] || !Zero(p.data() + 16, 112)) {
        return false;
    }
    m.header = {U32(p.data() + 4), U32(p.data() + 12)};
    return true;
}
inline bool Encode(const ReadyReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::Ready, m.header.transmit_timestamp_us,
           m.header.session_id);
    Put32(p.data() + 16, m.capabilities);
    Put32(p.data() + 20, m.request_bytes);
    Put32(p.data() + 24, m.max_report_bytes);
    Put32(p.data() + 28, m.can_channel_mask);
    Put32(p.data() + 36, m.protection_signature);
    Put32(p.data() + 40, m.max_lease_us);
    p[32] = m.max_can_frames;
    p[33] = m.protection_frame_count;
    p[34] = m.protection_flags;
    return true;
}
inline bool Encode(const RequestResultReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::Result, m.header.transmit_timestamp_us,
           m.header.session_id);
    Put32(p.data() + 16, m.request_sequence);
    p[20] = uint8_t(m.request_kind);
    p[21] = m.armed;
    Put32(p.data() + 24, uint32_t(m.result_code));
    return true;
}
inline bool Encode(const ImuSampleReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::ImuSampleReport, m.header.transmit_timestamp_us,
           m.header.session_id);
    p[10] = m.spi_bytes.size();
    Put32(p.data() + 16, m.sample_timestamp_us);
    std::memcpy(p.data() + 20, m.spi_bytes.data(), m.spi_bytes.size());
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    uint32_t temperature_bits;
    std::memcpy(&temperature_bits, &m.temperature_c, 4);
    Put32(p.data() + 48, temperature_bits);
    Put32(p.data() + 52, m.sample_sequence);
    return true;
}
inline bool Encode(const SessionStatusReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::Status, m.header.transmit_timestamp_us,
           m.header.session_id);
    p[11] = m.channel;
    p[16] = m.armed;
    p[17] = m.configured;
    p[18] = uint8_t(m.stop_reason);
    Put32(p.data() + 20, m.rejected_requests);
    Put32(p.data() + 24, m.can_report_drops);
    Put32(p.data() + 28, m.sensor_report_drops);
    Put32(p.data() + 32, m.stop_errors);
    Put32(p.data() + 36, m.reply_drops);
    Put32(p.data() + 40, m.usb_send_errors);
    Put32(p.data() + 44, m.usb_receive_drops);
    Put32(p.data() + 48, m.usb_endpoint_errors);
    Put32(p.data() + 52, m.protection_signature);
    Put32(p.data() + 56, m.max_lease_us);
    return true;
}
inline bool Encode(const CanStatusReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::Status, m.header.transmit_timestamp_us,
           m.header.session_id);
    p[10] = 1;
    p[11] = m.channel;
    Put32(p.data() + 16, m.received);
    Put32(p.data() + 20, m.submitted);
    Put32(p.data() + 24, m.completed);
    Put32(p.data() + 28, m.errors);
    Put32(p.data() + 32, m.dropped);
    Put32(p.data() + 36, m.queue_depth);
    Put32(p.data() + 40, m.controller_state);
    Put32(p.data() + 44, m.tx_errors);
    Put32(p.data() + 48, m.rx_errors);
    return true;
}
inline bool Encode(const UartStatusReport& m, UsbInPacket& p) {
    p = {};
    Header(p.data(), Kind::Status, m.header.transmit_timestamp_us,
           m.header.session_id);
    p[10] = 2;
    p[11] = m.channel;
    Put32(p.data() + 16, m.received_blocks);
    Put32(p.data() + 20, m.dropped_blocks);
    Put32(p.data() + 24, m.completed_writes);
    Put32(p.data() + 28, m.tx_errors);
    return true;
}

inline bool Encode(const StatusRequest& m, UsbOutRequest& p) {
    if (uint8_t(m.target) > 2) {
        return false;
    }
    p = {};
    Header(p.data(), Kind::Status, m.header.request_sequence,
           m.header.session_id);
    p[10] = uint8_t(m.target);
    p[11] = m.channel;
    return true;
}
inline bool Decode(const UsbOutRequest& p, StatusRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::Status ||
        p[10] > 2 || !Zero(p.data() + 16, 112)) {
        return false;
    }
    m.header = {U32(p.data() + 4), U32(p.data() + 12)};
    m.target = StatusTarget(p[10]);
    m.channel = p[11];
    return true;
}
inline bool Encode(const ConfigureSafetyRequest& m, UsbOutRequest& p) {
    if (m.uart_stop_count > kMaxRecords || m.lease_us < 1000 ||
        m.lease_us > 1000000) {
        return false;
    }
    p = {};
    Header(p.data(), Kind::ConfigureSafety, m.header.request_sequence,
           m.header.session_id);
    Put32(p.data() + 16, m.lease_us);
    p[10] = m.uart_stop_count;
    for (size_t i = 0; i < m.uart_stop_count; ++i) {
        const auto& action = m.uart_stop_actions[i];
        CanFrame record{};
        record.channel = action.channel;
        record.length = action.length;
        record.flags = kUartAction;
        std::memcpy(record.data, action.data, 8);
        Encode(p.data() + 32 + i * 16, record);
    }
    return ValidRecords(p, true);
}
inline bool Decode(const UsbOutRequest& p, ConfigureSafetyRequest& m) {
    if (!HasHeader(p.data(), p.size()) || Kind(p[9]) != Kind::ConfigureSafety ||
        !ValidRecords(p, true)) {
        return false;
    }
    ConfigureSafetyRequest result{};
    result.header = {U32(p.data() + 4), U32(p.data() + 12)};
    result.lease_us = U32(p.data() + 16);
    for (size_t i = 0; i < p[10]; ++i) {
        const auto record = Decode(p.data() + 32 + i * 16);
        if (record.flags == kUartAction) {
            auto& action = result.uart_stop_actions[result.uart_stop_count++];
            action.channel = record.channel;
            action.length = record.length;
            std::memcpy(action.data, record.data, 8);
        } else {
            return false;
        }
    }
    m = result;
    return true;
}
}  // namespace dust::wire
