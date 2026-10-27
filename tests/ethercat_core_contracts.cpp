/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "ethercat/protocol.hpp"

using namespace dust::ecat;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

static Limits AllLimits() {
    return {4096, 4096, 24576, 15, 63, 63, true};
}
static Configuration Config() {
    Configuration c;
    c.can_tx = {1, 1};
    c.can_rx = {1, 0, 0, 0};
    c.uart[0] = {8, 8};
    return c;
}
static void Stage(ConfigurationTransaction& t, const Configuration& c) {
    const uint32_t values[] = {
        c.signature,        c.cycle_us,         c.watchdog_us,
        c.can_tx[0],        c.can_tx[1],        c.can_rx[0],
        c.can_rx[1],        c.can_rx[2],        c.can_rx[3],
        c.uart[0].tx_bytes, c.uart[0].rx_bytes, c.uart[1].tx_bytes,
        c.uart[1].rx_bytes, c.uart[2].tx_bytes, c.uart[2].rx_bytes,
        c.uart[3].tx_bytes, c.uart[3].rx_bytes, c.uart[4].tx_bytes,
        c.uart[4].rx_bytes, c.uart[5].tx_bytes, c.uart[5].rx_bytes,
        uint32_t(c.imu)};
    for (unsigned i = 22; i > 0; --i) {
        CHECK(t.Write(uint8_t(i), values[i - 1]) == Error::None);
    }
}
struct Guard {
    unsigned stops = 0;
    static bool Validate(void*, unsigned source, const CanFrame& f) {
        return f.id == (source ? 0x200U : 0x100U) && f.bus == source &&
               f.length == 8;
    }
    static void Stop(void* context, Error) {
        static_cast<Guard*>(context)->stops++;
    }
};
struct Fixture {
    Guard guard;
    Core core;
    Configuration config;
    Layout layout;
    ConfigurationTransaction transaction;
    std::vector<uint8_t> rx, tx;
    Actions actions;
    Core::Feedback feedback;
    explicit Fixture(bool protected_backend = true,
                     uint64_t boot = 0x123456789abcdef0ULL,
                     Configuration candidate = Config())
        : core(boot, {&guard, Guard::Validate, Guard::Stop, protected_backend}),
          config(candidate) {
        CHECK(BuildLayout(config, AllLimits(), layout) == Error::None);
        config.signature = layout.signature;
        Stage(transaction, config);
        CHECK(core.Configure(transaction, AllLimits(), layout.rx, layout.tx) ==
              Error::None);
        rx.resize(layout.rx.bytes);
        tx.resize(layout.tx.bytes);
    }
    void Header(uint32_t exchange, uint32_t flags = kArm, uint32_t sources = 0,
                uint64_t session = 9) {
        std::fill(rx.begin(), rx.end(), 0);
        Put32(rx.data(), kMagic);
        Put16(rx.data() + 4, 1);
        Put64(rx.data() + 8, session);
        Put64(rx.data() + 16, flags == kDiscover ? 0 : 0x123456789abcdef0ULL);
        Put32(rx.data() + 24, exchange);
        Put32(rx.data() + 28, flags);
        Put32(rx.data() + 32, layout.signature);
        Put32(rx.data() + 36, sources);
        Put32(rx.data() + 40, 1000);
    }
    void Batch(unsigned source, uint32_t sequence, uint32_t remaining = 10000,
               unsigned count = 1) {
        auto* p = rx.data() + layout.source[source];
        Put32(p, sequence);
        Put32(p + 4, remaining);
        Put16(p + 8, 1);
        p[10] = uint8_t(count);
        p[11] = uint8_t(source);
        if (count) {
            p[16] = uint8_t(source);
            p[17] = 8;
            Put32(p + 20, source ? 0x200 : 0x100);
            p[24] = uint8_t(sequence);
        }
    }
    Error Process(uint64_t now = 1000, bool op = true) {
        return core.Process({rx.data(), rx.size()}, now, op, actions);
    }
    void Uart(uint32_t sequence, uint16_t flags = kDataValid,
              uint32_t ack = 0) {
        auto* p = rx.data() + layout.uart_rx[0];
        Put32(p, sequence);
        Put32(p + 4, ack);
        Put16(p + 14, flags);
        if (flags & kDataValid) {
            Put16(p + 12, 3);
            std::memcpy(p + 16, "abc", 3);
        }
    }
    void Publish(uint64_t now) {
        CHECK(core.Publish({tx.data(), tx.size()}, now, feedback) ==
              Error::None);
    }
};
static void ConfigurationContracts() {
    Configuration c = Config();
    Layout l;
    auto limits = AllLimits();
    CHECK(BuildLayout(c, limits, l) == Error::None);
    ConfigurationTransaction t;
    CHECK(t.Commit(limits, l.rx, l.tx, l) == Error::Incomplete);
    CHECK(t.Write(0, 0) == Error::Argument);
    CHECK(t.Write(23, 0) == Error::Argument);
    CHECK(t.Write(4, 256) == Error::Argument);
    CHECK(t.Write(10, 65536) == Error::Argument);
    CHECK(t.Write(2, 500) == Error::Argument);
    CHECK(t.Write(3, 19999) == Error::Argument);
    c.signature = l.signature;
    Stage(t, c);
    Layout actual = l;
    actual.rx.entries[0].bits = 8;
    CHECK(t.Commit(limits, actual.rx, actual.tx, l) == Error::Mapping);
    CHECK(BuildLayout(c, limits, actual) == Error::None);
    CHECK(t.Commit(limits, actual.rx, actual.tx, l) == Error::None);
    CHECK(t.Commit(limits, actual.rx, actual.tx, actual) == Error::Argument);
    actual.rx.mappings[0].count--;
    CHECK(t.Commit(limits, actual.rx, actual.tx, l) == Error::Mapping);
    CHECK(BuildLayout(c, limits, actual) == Error::None);
    CHECK(t.Write(1, c.signature ^ 1) == Error::None);
    CHECK(t.Commit(limits, actual.rx, actual.tx, l) == Error::Signature);
    limits.process_ram_bytes = 3 * (actual.rx.bytes + actual.tx.bytes) - 1;
    CHECK(BuildLayout(c, limits, l) == Error::Capacity);
    limits = AllLimits();
    limits.uart_tx_mask = 0;
    CHECK(BuildLayout(c, limits, l) == Error::Capacity);
    limits = AllLimits();
    limits.imu = false;
    c.imu = true;
    CHECK(BuildLayout(c, limits, l) == Error::Capacity);
    t.Clear();
    CHECK(t.Commit(AllLimits(), actual.rx, actual.tx, l) == Error::Incomplete);
    Core invalid_boot(0);
    CHECK(invalid_boot.Configure(t, AllLimits(), actual.rx, actual.tx) ==
          Error::Argument);
}
static void SessionContracts() {
    Fixture f;
    f.Header(1, kDiscover);
    CHECK(f.Process() == Error::None);
    CHECK(f.core.state() == BusinessState::Ready);
    f.Publish(1000);
    CHECK(U64(f.tx.data() + 8) == 0x123456789abcdef0ULL);
    CHECK(U64(f.tx.data() + 16) == 0);
    CHECK(U32(f.tx.data() + 36) & kProtectionCapability);
    f.Header(2);
    CHECK(f.Process(2000) == Error::None);
    CHECK(f.core.state() == BusinessState::Active);
    CHECK(f.core.Configure(f.transaction, AllLimits(), f.layout.rx,
                           f.layout.tx) == Error::State);
    f.core.Tick(21999, true);
    CHECK(f.core.state() == BusinessState::Active);
    f.core.Tick(22000, true);
    CHECK(f.core.fault() == Error::Lease);
    CHECK(f.guard.stops == 1);
    f.Header(3, kArm, 0, 10);
    CHECK(f.Process(23000) == Error::Lease);
    CHECK(f.guard.stops == 1);
    f.Publish(23000);
    CHECK(U32(f.tx.data() + 40) == 3);
    CHECK(U32(f.tx.data() + 44) != 0);
    Fixture no_guard(false);
    no_guard.Publish(1);
    CHECK(!(U32(no_guard.tx.data() + 36) & kProtectionCapability));
    no_guard.Header(1);
    CHECK(no_guard.Process() == Error::Protection);
    Fixture boot;
    boot.Header(1);
    Put64(boot.rx.data() + 16, 0x99);
    CHECK(boot.Process() == Error::Session);
    Fixture session;
    session.Header(1);
    CHECK(session.Process() == Error::None);
    session.Header(2, kArm, 0, 10);
    CHECK(session.Process(2000) == Error::Session);
    Fixture transport;
    transport.Header(1);
    CHECK(transport.Process() == Error::None);
    transport.core.Tick(1001, false);
    CHECK(transport.core.fault() == Error::Transport);
    Fixture stop;
    stop.Header(1);
    CHECK(stop.Process() == Error::None);
    stop.Header(2, kStop);
    CHECK(stop.Process(2000) == Error::None);
    CHECK(stop.core.state() == BusinessState::Protected);
    stop.Header(3);
    CHECK(stop.Process(3000) == Error::State);
}
static void LeaseContracts() {
    Fixture f;
    f.Header(1, kArm, 3);
    f.Batch(0, 10, 5000);
    f.Batch(1, 10, 15000);
    CHECK(f.Process() == Error::None);
    CHECK(f.actions.can[0].valid && f.actions.can[1].valid);
    CHECK(f.actions.can[0].deadline_us == 6000);
    f.Header(2, kArm, 3);
    f.Batch(0, 10, 5000);
    f.Batch(1, 11, 15000);
    CHECK(f.Process(3000) == Error::None);
    CHECK(!f.actions.can[0].valid);
    CHECK(f.actions.can[1].valid);
    f.Uart(1);  // UART 活动也不能续 chassis 的独立租约。
    f.Header(3, kArm, 3);
    f.Batch(0, 10, 5000);
    f.Batch(1, 12, 15000);
    f.Uart(1);
    CHECK(f.Process(5000) == Error::None);
    CHECK(f.core.CommitUart(0, 1));
    f.core.Tick(6000, true);
    CHECK(f.core.fault() == Error::Lease);
    Fixture duplicate;
    duplicate.Header(1, kArm, 1);
    duplicate.Batch(0, 1, 10000);
    CHECK(duplicate.Process() == Error::None);
    CHECK(duplicate.Process(9000) == Error::None);
    CHECK(!duplicate.actions.can[0].valid);
    duplicate.core.Tick(11000, true);
    CHECK(duplicate.core.fault() == Error::Lease);
    Fixture empty;
    empty.Header(1, kArm, 1);
    empty.Batch(0, 1, 10000, 0);
    CHECK(empty.Process() == Error::None);
    CHECK(empty.actions.can[0].valid && !empty.actions.can[0].count);
    Fixture initial;
    initial.Header(1, kArm, 1);
    CHECK(initial.Process() == Error::None);
    initial.core.Tick(21000, true);
    CHECK(initial.core.fault() == Error::Lease);
    Fixture changed;
    changed.Header(1, kArm, 1);
    changed.Batch(0, 7);
    CHECK(changed.Process() == Error::None);
    changed.Header(2, kArm, 1);
    changed.Batch(0, 7);
    changed.rx[changed.layout.source[0] + 24]++;
    CHECK(changed.Process(2000) == Error::Sequence);
    Fixture invalid;
    invalid.Header(1, kArm, 3);
    invalid.Batch(0, 1);
    invalid.Batch(1, 1);
    Put32(invalid.rx.data() + invalid.layout.source[1] + 20, 0x123);
    CHECK(invalid.Process() == Error::Protection);
    CHECK(!invalid.actions.can[0].valid);
    Fixture wrap;
    wrap.Header(0xfffffffeU, kArm, 1);
    wrap.Batch(0, 0xfffffffeU);
    CHECK(wrap.Process() == Error::None);
    wrap.Header(0xffffffffU, kArm, 1);
    wrap.Batch(0, 0xffffffffU);
    CHECK(wrap.Process(2000) == Error::None);
    wrap.Header(0, kArm, 1);
    wrap.Batch(0, 0);
    CHECK(wrap.Process(3000) == Error::None);
    CHECK(wrap.actions.can[0].valid);
    CHECK(!Newer(0x80000000U, 0));
}
static void UartContracts() {
    Fixture f;
    f.Header(1);
    f.Uart(1);
    CHECK(f.Process() == Error::None);
    CHECK(f.actions.uart[0].valid);
    CHECK(!f.core.UartAckValid(0));
    CHECK(!f.core.CommitUart(0, 2));
    CHECK(f.core.CommitUart(0, 1));
    f.Header(2);
    f.Uart(1);
    CHECK(f.Process(2000) == Error::None);
    CHECK(!f.actions.uart[0].valid);
    f.Header(3);
    f.Uart(3);
    CHECK(f.Process(3000) == Error::None);
    CHECK(!f.actions.uart[0].valid);
    CHECK(f.core.UartAck(0) == 1);
    f.Header(4);
    f.Uart(2);
    CHECK(f.Process(4000) == Error::None);
    CHECK(f.core.CommitUart(0, 2));
    const uint8_t bytes[] = {7, 8, 9};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(f.core.QueueUart(0, {bytes, 3}, 4000, 4000));
    }
    CHECK(!f.core.QueueUart(0, {bytes, 3}, 4000, 4000));
    for (unsigned i = 0; i < 5; ++i) {
        f.Publish(4001 + i);
        const auto* p = f.tx.data() + f.layout.uart_tx[0];
        CHECK(U32(p) == i % 4 + 1);
        CHECK(U32(p + 4) == 2);
        CHECK(U16(p + 14) == 3);
    }
    f.Header(5);
    f.Uart(0, kAckValid, 2);
    CHECK(f.Process(5000) == Error::None);
    f.Publish(5001);
    CHECK(U32(f.tx.data() + f.layout.uart_tx[0]) == 3);
    f.Header(6);
    f.Uart(0, kAckValid, 4);
    CHECK(f.Process(6000) == Error::None);
    f.Publish(6001);
    CHECK(!(U16(f.tx.data() + f.layout.uart_tx[0] + 14) & kDataValid));
    CHECK(f.core.QueueUart(0, {bytes, 3}, 6000, 6000, true));
    f.Publish(6002);
    CHECK(U16(f.tx.data() + f.layout.uart_tx[0] + 14) & kDiscontinuity);
    f.core.Tick(26000, true);
    CHECK(f.core.state() == BusinessState::Protected);
    Fixture unsent;
    unsent.Header(1);
    CHECK(unsent.Process() == Error::None);
    CHECK(unsent.core.QueueUart(0, {bytes, 3}, 1000, 1000));
    unsent.Header(2);
    unsent.Uart(0, kAckValid, 1);
    CHECK(unsent.Process(2000) == Error::Sequence);
    Fixture timeout;
    timeout.Header(1);
    CHECK(timeout.Process() == Error::None);
    CHECK(timeout.core.QueueUart(0, {bytes, 3}, 1000, 1000));
    timeout.Publish(1001);
    timeout.Header(2);
    CHECK(timeout.Process(19000) == Error::None);
    timeout.core.Tick(21000, true);
    CHECK(timeout.core.fault() == Error::UartTimeout);
    Fixture bad;
    bad.Header(1, kArm, 1);
    bad.Batch(0, 1);
    bad.Uart(1);
    Put16(bad.rx.data() + bad.layout.uart_rx[0] + 12, 9);
    CHECK(bad.Process() == Error::Wire);
    CHECK(!bad.actions.can[0].valid);
}
static void WireContracts() {
    Fixture f;
    f.Header(1);
    Put32(f.rx.data() + 44, 8);
    f.rx[48] = 12;
    CHECK(f.Process() == Error::None);
    CHECK(f.actions.status_sequence == 8);
    f.core.CompleteStatus(7, -22, {1, 2, 3});
    f.Publish(1000);
    CHECK(U32(f.tx.data() + 56) == 0);
    f.core.CompleteStatus(8, -22, {1, 2, 3});
    Core::CanEvent event{0x321, 10, 1000, 8, {1, 2, 3, 4, 5, 6, 7, 8}};
    f.feedback.can[0] = {3, 2, &event, 1};
    f.Publish(1001);
    CHECK(U32(f.tx.data() + 56) == 8);
    const auto* diagnostic = f.tx.data() + f.layout.diagnostics;
    CHECK(U32(diagnostic + 208) == uint32_t(-22));
    CHECK(U32(diagnostic + 220) == 3);
    const auto* can = f.tx.data() + f.layout.feedback[0];
    CHECK(U32(can) == 3 && U32(can + 4) == 2 && U16(can + 8) == 1);
    CHECK(U32(can + 16) == 0x321 && U32(can + 20) == 10 && can[28] == 8);
    for (size_t n = 0; n < 64; ++n) {
        RxHeader h;
        CHECK(DecodeHeader({f.rx.data(), n}, h) == Error::Wire);
    }
    Fixture reserved;
    reserved.Header(1);
    reserved.rx[63] = 1;
    CHECK(reserved.Process() == Error::Wire);
    Fixture mixed;
    mixed.Header(1, kDiscover | kArm);
    CHECK(mixed.Process() == Error::Wire);
    Fixture signature;
    signature.Header(1);
    signature.rx[32] ^= 1;
    CHECK(signature.Process() == Error::Signature);
    Fixture replay;
    replay.Header(1);
    CHECK(replay.Process() == Error::None);
    replay.rx[24] = 0;
    CHECK(replay.Process(2000) == Error::Sequence);
    Fixture rewritten;
    rewritten.Header(1);
    CHECK(rewritten.Process() == Error::None);
    rewritten.rx[44] = 9;
    CHECK(rewritten.Process(2000) == Error::Sequence);
    // 从站时间低 32 位回绕，不影响真实的样本年龄。
    event.capture_us = 0xfffffffeU;
    f.feedback.can[0] = {4, 2, &event, 1};
    f.Publish(0x100000005ULL);
}
static void AdditionalContracts() {
    Guard lifecycle;
    {
        Fixture mapping;
        Core owned(0x123456789abcdef0ULL,
                   {&lifecycle, Guard::Validate, Guard::Stop, true});
        CHECK(owned.Configure(mapping.transaction, AllLimits(),
                              mapping.layout.rx,
                              mapping.layout.tx) == Error::None);
        mapping.Header(1);
        CHECK(owned.Process({mapping.rx.data(), mapping.rx.size()}, 1000, true,
                            mapping.actions) == Error::None);
    }
    CHECK(lifecycle.stops == 1);

    Configuration c;
    c.uart[0] = {0, 8};
    Fixture one_way(true, 0x123456789abcdef0ULL, c);
    CHECK(one_way.layout.rx.bytes == 80);
    one_way.Header(1);
    CHECK(one_way.Process() == Error::None);
    const uint8_t data[] = {1, 2};
    CHECK(one_way.core.QueueUart(0, {data, 2}, 1000, 1000));
    one_way.Publish(1001);
    one_way.Header(2);
    one_way.Uart(0, kAckValid, 1);
    CHECK(one_way.Process(2000) == Error::None);
    one_way.Publish(2001);
    CHECK(!(U16(one_way.tx.data() + one_way.layout.uart_tx[0] + 14) &
            kDataValid));

    c.uart[0] = {8, 8};
    c.uart[1] = {8, 8};
    Fixture independent(true, 0x123456789abcdef0ULL, c);
    independent.Header(1);
    CHECK(independent.Process() == Error::None);
    CHECK(independent.core.QueueUart(0, {data, 2}, 1000, 1000));
    CHECK(independent.core.QueueUart(1, {data, 2}, 1000, 1000));
    independent.Publish(1001);
    independent.Header(2);
    auto* second = independent.rx.data() + independent.layout.uart_rx[1];
    Put32(second + 4, 1);
    Put16(second + 14, kAckValid);
    CHECK(independent.Process(2000) == Error::None);
    independent.Publish(2001);
    CHECK(U16(independent.tx.data() + independent.layout.uart_tx[0] + 14) &
          kDataValid);
    CHECK(!(U16(independent.tx.data() + independent.layout.uart_tx[1] + 14) &
            kDataValid));

    Fixture wrapped;
    wrapped.Header(1);
    CHECK(wrapped.Process(0x100000000ULL) == Error::None);
    CHECK(wrapped.core.QueueUart(0, {data, 2}, 0xfffffff0U, 0x100000000ULL));
    CHECK(!wrapped.core.QueueUart(0, {data, 2}, 1, 0x100000000ULL));
    wrapped.Publish(0x100000001ULL);
    CHECK(U32(wrapped.tx.data() + wrapped.layout.uart_tx[0] + 8) ==
          0xfffffff0U);

    Fixture aged;
    aged.Header(1);
    CHECK(aged.Process(10000) == Error::None);
    CHECK(aged.core.QueueUart(0, {data, 2}, 1, 10000));
    aged.Header(2);
    CHECK(aged.Process(19000) == Error::None);
    aged.core.Tick(20001, true);
    CHECK(aged.core.fault() == Error::UartTimeout);

    Fixture mask;
    mask.Header(1, kArm, 1);
    CHECK(mask.Process() == Error::None);
    mask.Header(2, kArm, 3);
    CHECK(mask.Process(2000) == Error::Session);
    Fixture invalid_feedback;
    invalid_feedback.Header(1);
    CHECK(invalid_feedback.Process() == Error::None);
    invalid_feedback.feedback.can[0].count = 2;
    CHECK(invalid_feedback.core.Publish(
              {invalid_feedback.tx.data(), invalid_feedback.tx.size()}, 1001,
              invalid_feedback.feedback) == Error::Capacity);
    CHECK(invalid_feedback.core.state() == BusinessState::Protected);

    Fixture size;
    const size_t expected = size.rx.size();
    for (size_t n = 0; n < expected; ++n) {
        Fixture truncated;
        truncated.Header(1);
        CHECK(truncated.core.Process({truncated.rx.data(), n}, 1000, true,
                                     truncated.actions) == Error::Wire);
        CHECK(!truncated.actions.can[0].valid);
    }
}
int main() {
    AdditionalContracts();
    ConfigurationContracts();
    SessionContracts();
    LeaseContracts();
    UartContracts();
    WireContracts();
    std::printf("EtherCAT core contracts PASS; sizeof(Core)=%zu bytes\n",
                sizeof(Core));
}
