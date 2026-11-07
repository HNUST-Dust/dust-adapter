// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace dust::device_pdo {

constexpr size_t kBytes = 10;
constexpr size_t kPayloadBytes = 8;
constexpr size_t kImuBytes = 38;
constexpr size_t kImuParts = (kImuBytes + kPayloadBytes - 1) / kPayloadBytes;

using Record = std::array<uint8_t, kBytes>;
using Payload = std::array<uint8_t, kPayloadBytes>;
using ImuSample = std::array<uint8_t, kImuBytes>;

enum class Direction { Rx, Tx };
enum class Device : uint8_t {
    RightWheel,
    RightJointB,
    RightJointD,
    LeftWheel,
    LeftJointB,
    LeftJointD,
};
enum class Type : uint8_t {
    MotorFirst = 0x10,
    ImuFirst = 0x20,
    Discover = 0x30,
    Ready = 0x31,
    Arm = 0x32,
    Stop = 0x33,
    Status = 0x34,
};

struct CanRoute {
    uint8_t bus;
    uint16_t command;
    uint16_t feedback;
};

#if defined(CONFIG_ECAT_CAN_ISOLATION) || defined(DUST_CAN_ISOLATION)
constexpr std::array<CanRoute, 6> kRoutes{{
    {1, 0x200, 0x201}, {0, 0x001, 0x011}, {0, 0x002, 0x012},
    {2, 0x200, 0x201}, {3, 0x000, 0x010}, {3, 0x003, 0x013},
}};
constexpr std::array<uint8_t, 4> kTxBuses{0, 1, 2, 3};
constexpr std::array<uint8_t, 4> kCanRxCounts{2, 1, 1, 2};
#else
constexpr std::array<CanRoute, 6> kRoutes{{
    {0, 0x200, 0x201}, {0, 0x001, 0x011}, {0, 0x002, 0x012},
    {3, 0x200, 0x201}, {3, 0x000, 0x010}, {3, 0x003, 0x013},
}};
constexpr std::array<uint8_t, 2> kTxBuses{0, 3};
constexpr std::array<uint8_t, 4> kCanRxCounts{3, 0, 0, 3};
#endif

constexpr size_t BusLane(uint8_t bus) {
    for (size_t lane = 0; lane < kTxBuses.size(); ++lane)
        if (kTxBuses[lane] == bus) return lane;
    return kTxBuses.size();
}

inline const CanRoute* Route(Device device) {
    const auto index = size_t(device);
    return index < kRoutes.size() ? &kRoutes[index] : nullptr;
}

constexpr size_t kMotorBatchBytes = kRoutes.size() * kBytes;
constexpr uint32_t kMotorBatchSignature = 0x44543630;

struct View {
    Type type;
    uint8_t sequence;
    Payload payload;
};

inline bool Decode(Direction direction, const uint8_t* data, size_t size,
                   View& view) {
    if (!data || size != kBytes) {
        return false;
    }
    const auto type = Type(data[0]);
    const auto motor = unsigned(data[0]) - unsigned(Type::MotorFirst);
    const auto part = unsigned(data[0]) - unsigned(Type::ImuFirst);
    const bool control =
        direction == Direction::Rx
            ? type == Type::Discover || type == Type::Arm || type == Type::Stop
            : type == Type::Ready || type == Type::Status;
    if (motor >= kRoutes.size() && !control &&
        (direction != Direction::Tx || part >= kImuParts)) {
        return false;
    }
    if (direction == Direction::Tx && part == kImuParts - 1) {
        const size_t used = kImuBytes - part * kPayloadBytes;
        if (!std::all_of(data + 2 + used, data + kBytes,
                         [](uint8_t byte) { return byte == 0; })) {
            return false;
        }
    }
    view.type = type;
    view.sequence = data[1];
    std::copy_n(data + 2, kPayloadBytes, view.payload.begin());
    return true;
}

inline Record Encode(Type type, uint8_t sequence, const Payload& payload) {
    Record record{};
    record[0] = uint8_t(type);
    record[1] = sequence;
    std::copy(payload.begin(), payload.end(), record.begin() + 2);
    return record;
}

inline bool EncodeMotor(Device device, uint8_t sequence, const Payload& payload,
                        Record& record) {
    if (!Route(device)) {
        return false;
    }
    record = Encode(Type(unsigned(Type::MotorFirst) + unsigned(device)),
                    sequence, payload);
    return true;
}

inline bool DecodeMotorBatch(const uint8_t* data, size_t size,
                             uint8_t& sequence) {
    if (!data || size != kMotorBatchBytes) {
        return false;
    }
    const uint8_t candidate = data[1];
    for (unsigned slot = 0; slot < kRoutes.size(); ++slot) {
        View view{};
        if (!Decode(Direction::Rx, data + slot * kBytes, kBytes, view) ||
            unsigned(view.type) != unsigned(Type::MotorFirst) + slot ||
            view.sequence != candidate) {
            return false;
        }
    }
    sequence = candidate;
    return true;
}

struct MotorCommand {
    uint8_t bus = 0;
    uint16_t id = 0;
    Payload payload{};
};

inline bool DecodeMotorCommands(const uint8_t* data, size_t size,
                                uint8_t& sequence,
                                std::array<MotorCommand, 6>& commands) {
    if (!DecodeMotorBatch(data, size, sequence)) {
        return false;
    }
    for (unsigned i = 0; i < commands.size(); ++i) {
        commands[i].bus = kRoutes[i].bus;
        commands[i].id = kRoutes[i].command;
        std::copy_n(data + i * kBytes + 2, kPayloadBytes,
                    commands[i].payload.begin());
    }
    return true;
}

inline bool EncodeImuPart(const ImuSample& sample, uint8_t sequence,
                          size_t part, Record& record) {
    if (part >= kImuParts) {
        return false;
    }
    const size_t offset = part * kPayloadBytes;
    Payload payload{};
    std::copy_n(sample.begin() + offset,
                std::min(kPayloadBytes, sample.size() - offset),
                payload.begin());
    record = Encode(Type(unsigned(Type::ImuFirst) + part), sequence, payload);
    return true;
}

inline bool Newer(uint8_t sequence, uint8_t previous) {
    const auto delta = uint8_t(sequence - previous);
    return delta != 0 && delta < 128;
}

// The owner resets the assembler on Stop, disconnect and session replacement.
// A missing part abandons the sample. A repeated part never extends its
// deadline.
class ImuAssembler {
   public:
    explicit ImuAssembler(uint32_t timeout_us) : timeout_us_(timeout_us) {}

    void Reset() {
        next_part_ = 0;
        have_sequence_ = false;
    }

    bool Accept(const View& view, uint32_t now_us, ImuSample& sample) {
        const auto part = unsigned(view.type) - unsigned(Type::ImuFirst);
        if (part >= kImuParts) {
            return false;
        }
        if (next_part_ && uint32_t(now_us - started_us_) >= timeout_us_) {
            next_part_ = 0;
        }
        if (part == 0) {
            if (have_sequence_ && !Newer(view.sequence, sequence_)) {
                return false;
            }
            have_sequence_ = true;
            sequence_ = view.sequence;
            started_us_ = now_us;
        } else {
            if (!next_part_ || view.sequence != sequence_ ||
                part < next_part_) {
                return false;
            }
            if (part != next_part_) {
                next_part_ = 0;
                return false;
            }
        }
        const size_t offset = part * kPayloadBytes;
        std::copy_n(view.payload.begin(),
                    std::min(kPayloadBytes, pending_.size() - offset),
                    pending_.begin() + offset);
        next_part_ = part + 1;
        if (next_part_ != kImuParts) {
            return false;
        }
        sample = pending_;
        next_part_ = 0;
        return true;
    }

   private:
    const uint32_t timeout_us_;
    ImuSample pending_{};
    uint32_t started_us_ = 0;
    size_t next_part_ = 0;
    uint8_t sequence_ = 0;
    bool have_sequence_ = false;
};

}  // namespace dust::device_pdo
