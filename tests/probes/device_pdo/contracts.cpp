// SPDX-License-Identifier: Apache-2.0
#include <cassert>

#include "common/device_pdo.hpp"

using namespace dust::device_pdo;

int main() {
    Record record{};
    View view{};
    const Payload motor{1, 2, 3, 4, 5, 6, 7, 8};
    for (unsigned index = 0; index < kRoutes.size(); ++index) {
        assert(EncodeMotor(Device(index), 42, motor, record));
        assert(Decode(Direction::Rx, record.data(), record.size(), view));
        assert(Decode(Direction::Tx, record.data(), record.size(), view));
        assert(view.sequence == 42 && view.payload == motor);
        assert(!Decode(Direction::Tx, record.data(), 9, view));
    }
    assert(!EncodeMotor(Device(6), 0, motor, record));
    std::array<uint8_t, kMotorBatchBytes> batch{};
    for (unsigned slot = 0; slot < kRoutes.size(); ++slot) {
        assert(EncodeMotor(Device(slot), 42, motor, record));
        std::copy(record.begin(), record.end(), batch.begin() + slot * kBytes);
    }
    uint8_t sequence = 0;
    assert(DecodeMotorBatch(batch.data(), batch.size(), sequence) &&
           sequence == 42);
    assert(!DecodeMotorBatch(batch.data(), batch.size() - 1, sequence));
    batch[11] = 43;
    assert(!DecodeMotorBatch(batch.data(), batch.size(), sequence));
    batch[11] = 42;
    batch[10] = batch[0];
    assert(!DecodeMotorBatch(batch.data(), batch.size(), sequence));
    record = Encode(Type::Stop, 43, {});
    assert(Decode(Direction::Rx, record.data(), record.size(), view));
    assert(!Decode(Direction::Tx, record.data(), record.size(), view));

    ImuSample source{}, received{};
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = uint8_t(i + 1);
    }
    ImuAssembler assembler(2000);
    auto accept = [&](size_t part, uint8_t sequence, uint32_t now) {
        assert(EncodeImuPart(source, sequence, part, record));
        assert(Decode(Direction::Tx, record.data(), record.size(), view));
        assert(!Decode(Direction::Rx, record.data(), record.size(), view));
        return assembler.Accept(view, now, received);
    };
    for (size_t part = 0; part < kImuParts; ++part) {
        assert(accept(part, 255, uint32_t(part * 125)) ==
               (part == kImuParts - 1));
    }
    assert(received == source);
    assert(!accept(0, 255, 750));  // replay
    assert(!accept(0, 0, 1000));   // sequence wrap
    assert(!accept(0, 0, 1100));   // repeated first part
    assert(!accept(2, 0, 1200));   // missing part
    assert(!accept(1, 0, 1300));
    assert(!accept(4, 0, 1400));
    assert(!accept(0, 1, 2000));
    assert(!accept(1, 1, 4000));  // expired
    assembler.Reset();
    for (size_t part = 0; part < kImuParts; ++part) {
        assert(accept(part, 1, uint32_t(5000 + part * 125)) ==
               (part == kImuParts - 1));
    }
    record.back() = 1;  // final fragment padding
    assert(!Decode(Direction::Tx, record.data(), record.size(), view));
}
