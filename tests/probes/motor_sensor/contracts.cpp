// SPDX-License-Identifier: Apache-2.0
#include <cassert>

#include "common/device_sensor_page.hpp"

namespace wire = dust::device_sensor_page;

int main() {
    namespace motor = dust::device_pdo;
#if defined(DUST_CAN_ISOLATION) || defined(CONFIG_ECAT_CAN_ISOLATION)
    static_assert(wire::kSignature == 0x44544732);
    static_assert(motor::kTxBuses.size() == 4);
    static_assert(motor::kRoutes[0].bus == 1 && motor::kRoutes[3].bus == 2);
    static_assert(motor::kCanRxCounts[0] == 2 && motor::kCanRxCounts[1] == 1 &&
                  motor::kCanRxCounts[2] == 1 && motor::kCanRxCounts[3] == 2);
#else
    static_assert(wire::kSignature == 0x44544731);
    static_assert(motor::kTxBuses.size() == 2);
    static_assert(motor::kRoutes[0].bus == 0 && motor::kRoutes[3].bus == 3);
#endif
    static_assert(motor::kRoutes[1].bus == 0 && motor::kRoutes[2].bus == 0 &&
                  motor::kRoutes[4].bus == 3 && motor::kRoutes[5].bus == 3);
    for (unsigned i=0;i<motor::kTxBuses.size();++i)
        assert(motor::BusLane(motor::kTxBuses[i]) == i);
    assert(motor::BusLane(255) == motor::kTxBuses.size());
    std::array<uint8_t, motor::kMotorBatchBytes> motor_page{};
    for (unsigned i = 0; i < motor::kRoutes.size(); ++i) {
        motor::Payload payload{};
        if (i != unsigned(motor::Device::RightWheel) &&
            i != unsigned(motor::Device::LeftWheel)) {
            payload.fill(0xff);
            payload[7] = 0xfd;
        }
        motor::Record record;
        assert(motor::EncodeMotor(motor::Device(i), 42, payload, record));
        std::copy(record.begin(), record.end(), motor_page.begin() + i * motor::kBytes);
    }
    std::array<motor::MotorCommand, 6> commands{};
    uint8_t sequence = 0;
    assert(motor::DecodeMotorCommands(motor_page.data(), motor_page.size(), sequence, commands));
    assert(sequence == 42);
    for (unsigned i = 0; i < commands.size(); ++i) {
        assert(commands[i].bus == motor::kRoutes[i].bus);
        assert(commands[i].id == motor::kRoutes[i].command);
        assert(std::equal(commands[i].payload.begin(), commands[i].payload.end(),
                          motor_page.begin() + i * motor::kBytes + 2));
    }
    commands[0].id = 0x555;
    motor_page[5 * motor::kBytes + 1] = 41;
    assert(!motor::DecodeMotorCommands(motor_page.data(), motor_page.size(), sequence, commands));
    assert(commands[0].id == 0x555 && sequence == 42);
    assert(!motor::DecodeMotorCommands(motor_page.data(), motor_page.size() - 1, sequence, commands));

    dust::hi91::Compact fields{};
    for (unsigned i = 0; i < fields.size(); ++i) {
        fields[i] = uint8_t(i + 1);
    }
    const auto frame = dust::hi91::ExpandControlFields(fields, 123);
    assert(frame[14] == 123);
    assert(frame[18] == fields[0] && frame[29] == fields[11]);
    assert(frame[30] == fields[12] && frame[41] == fields[23]);
    assert(frame[54] == fields[24] && frame[65] == fields[35]);
    assert(frame[66] == fields[36] && frame[81] == fields[51]);
    auto damaged = frame;
    damaged[20] ^= 1;
    dust::hi91::Stream stream;
    dust::hi91::Compact decoded{};
    for (uint8_t byte : damaged) {
        assert(!stream.Accept(byte, decoded));
    }
    assert(stream.Rejected() == 1);
    for (unsigned i = 0; i < frame.size(); ++i) {
        assert(stream.Accept(frame[i], decoded) == (i + 1 == frame.size()));
    }
    assert(decoded == fields);

    wire::Page page{};
    std::array<uint8_t, 128> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
        bytes[i] = uint8_t(i);
    }
    wire::Assembler hi91(wire::Kind::Hi91, 2000);
    wire::Complete complete;
    auto feed = [&](size_t offset, uint32_t sequence, uint32_t now) {
        assert(wire::Encode(wire::Kind::Hi91, sequence, 19, false, bytes.data(),
                            bytes.size(), offset, page));
        return hi91.Accept(page.data(), page.size(), now, complete);
    };
    assert(!feed(0, 0xffffffff, 100));
    assert(!feed(48, 0xffffffff, 200));
    assert(feed(96, 0xffffffff, 300));
    assert(complete.length == 128 && complete.data == bytes &&
           complete.capture_us == 19);
    assert(!feed(0, 0, 400));
    assert(!feed(96, 0, 500));  // missing middle
    assert(hi91.ReceivedMask() == 5);
    assert(feed(48, 0, 600));  // only the missing fragment is needed
    assert(complete.length == 128 && complete.data == bytes);
    assert(!feed(0, 0, 610));  // no duplicate delivery
    assert(!feed(0, 1, 700));
    assert(!feed(48, 1, 2700));  // deadline reached
    assert(!feed(48, 2, 2800));
    assert(!feed(48, 2, 2900));  // duplicate
    assert(!feed(96, 2, 3000));
    assert(hi91.ReceivedMask() == 6);
    assert(feed(0, 2, 3100));
    assert(!feed(96, 2, 3200));  // no duplicate delivery

    for (auto kind : {wire::Kind::Imu, wire::Kind::ImuCompact, wire::Kind::Sbus}) {
        wire::Assembler assembler(kind, 2000);
        const auto length = wire::Limit(kind);
        assert(wire::Encode(kind, 4, 123, true, bytes.data(), length, 0, page));
        assert(assembler.Accept(page.data(), page.size(), 100, complete));
        assert(complete.length == length && complete.discontinuity);
        page.back() = 1;
        wire::Fragment f;
        assert(!wire::Decode(page.data(), page.size(), f));
    }
    assert(
        !wire::Encode(wire::Kind::Imu, 1, 0, false, bytes.data(), 37, 0, page));
}
