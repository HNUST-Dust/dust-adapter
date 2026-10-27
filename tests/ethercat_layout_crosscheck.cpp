/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdio>
#include <random>
#include <stdexcept>

#include "ethercat/pdo_dictionary.hpp"
#include "ethercat/protocol.hpp"
#include "hardware/ethercat/pdo_config.hpp"
#include "hardware/ethercat/wire.hpp"

namespace slave = dust::ecat;
namespace host = dust_control::ethercat;
static void Compare(const slave::ImageLayout& a,
                    const host::ProcessImageLayout& b) {
    assert(a.bytes == b.bytes && a.MappingCount() == b.mappings.size());
    unsigned n = 0;
    for (const auto& mapping : b.mappings) {
        assert(mapping.index == a.first_pdo + n / 255);
        for (const auto& e : mapping.entries) {
            assert(n < a.count);
            const auto& actual = a.entries[n++];
            assert(actual.index == e.object_index &&
                   actual.subindex == e.subindex &&
                   actual.bits == e.bit_length && actual.offset == e.offset);
        }
    }
    assert(n == a.count);
}
static void Replay(slave::PdoDictionary& dictionary,
                   const host::ProcessImageLayout& image, uint16_t assignment) {
    assert(dictionary.Write(assignment, 0, 1, 0) == slave::Error::None);
    unsigned pdo = 0;
    for (const auto& mapping : image.mappings) {
        assert(dictionary.Write(mapping.index, 0, 1, 0) == slave::Error::None);
        unsigned sub = 0;
        for (const auto& entry : mapping.entries) {
            const uint32_t value = (uint32_t(entry.object_index) << 16) |
                                   (uint32_t(entry.subindex) << 8) |
                                   entry.bit_length;
            assert(dictionary.Write(mapping.index, ++sub, 4, value) ==
                   slave::Error::None);
        }
        assert(dictionary.Write(mapping.index, 0, 1, sub) ==
               slave::Error::None);
        assert(dictionary.Write(assignment, ++pdo, 2, mapping.index) ==
               slave::Error::None);
    }
    assert(dictionary.Write(assignment, 0, 1, pdo) == slave::Error::None);
}
int main() {
    std::mt19937 random(0x5e31);
    unsigned valid = 0, rejected = 0, multi = 0;
    for (unsigned i = 0; i < 504; ++i) {
        slave::Configuration c;
        if (i == 0) {
            c.can_tx = {16, 16};
            c.can_rx.fill(8);
            c.uart[0] = {16, 16};
            c.uart[1] = {128, 128};
            for (unsigned p = 2; p < 6; ++p) {
                c.uart[p] = {32, 32};
            }
            c.imu = true;
        } else if (i == 1) {
            c.can_tx = {6, 0};
            c.can_rx = {0, 0, 3, 3};
            c.uart[0] = {0, 32};
            c.uart[1] = {0, 128};
        } else if (i == 2) {
            c.can_rx.fill(32);
        } else if (i > 3) {
            for (auto& n : c.can_tx) {
                n = random() % 17;
            }
            for (auto& n : c.can_rx) {
                n = random() % 33;
            }
            for (auto& cap : c.uart) {
                cap.tx_bytes = random() % 513;
                cap.rx_bytes = random() % 513;
            }
            c.imu = random() % 2;
        }
        if (i == 3) c.cycle_us = 125;
        slave::Layout actual;
        const auto result = slave::BuildLayout(
            c, {4096, 4096, 24576, 15, 63, 63, true}, actual);
        host::PdoConfig config;
        config.cycle_time_us = c.cycle_us;
        auto& adapter = config.adapters["probe"];
        adapter.can_tx_slots = c.can_tx;
        adapter.can_rx_slots = c.can_rx;
        adapter.onboard_imu = c.imu;
        for (unsigned p = 0; p < 6; ++p) {
            adapter.uart[p] = {c.uart[p].tx_bytes, c.uart[p].rx_bytes};
        }
        try {
            const auto expected =
                host::BuildPdoLayout(config).adapters.at("probe");
            assert(result == slave::Error::None);
            Compare(actual.rx, expected.rx);
            Compare(actual.tx, expected.tx);
            slave::PdoDictionary dictionary;
            Replay(dictionary, expected.rx, 0x1c12);
            Replay(dictionary, expected.tx, 0x1c13);
            slave::ImageLayout mapped_rx, mapped_tx;
            assert(dictionary.Snapshot(mapped_rx, mapped_tx) ==
                   slave::Error::None);
            Compare(mapped_rx, expected.rx);
            Compare(mapped_tx, expected.tx);
            slave::ConfigurationTransaction transaction;
            const uint32_t parameters[] = {
                actual.signature,   c.cycle_us,         c.watchdog_us,
                c.can_tx[0],        c.can_tx[1],        c.can_rx[0],
                c.can_rx[1],        c.can_rx[2],        c.can_rx[3],
                c.uart[0].tx_bytes, c.uart[0].rx_bytes, c.uart[1].tx_bytes,
                c.uart[1].rx_bytes, c.uart[2].tx_bytes, c.uart[2].rx_bytes,
                c.uart[3].tx_bytes, c.uart[3].rx_bytes, c.uart[4].tx_bytes,
                c.uart[4].rx_bytes, c.uart[5].tx_bytes, c.uart[5].rx_bytes,
                uint32_t(c.imu)};
            for (unsigned sub = 22; sub > 0; --sub) {
                assert(transaction.Write(sub, parameters[sub - 1]) ==
                       slave::Error::None);
            }
            slave::Layout committed;
            assert(transaction.Commit({4096, 4096, 24576, 15, 63, 63, true},
                                      mapped_rx, mapped_tx,
                                      committed) == slave::Error::None);
            assert(committed.signature == expected.layout_signature);
            mapped_rx.entries[0].index ^= 1;
            assert(transaction.Commit({4096, 4096, 24576, 15, 63, 63, true},
                                      mapped_rx, mapped_tx,
                                      committed) == slave::Error::Mapping);
            dictionary.Freeze();
            assert(dictionary.Write(0x1c12, 0, 1, 0) == slave::Error::State);
            if (actual.signature != expected.layout_signature) {
                std::fprintf(
                    stderr, "case %u signature mismatch slave=%08x host=%08x\n",
                    i, actual.signature, expected.layout_signature);
                return 1;
            }
            if (i == 0) {
                assert(actual.rx.bytes == 976 && actual.tx.bytes == 1536);
            }
            if (i == 1) {
                assert(actual.rx.bytes == 208 && actual.tx.bytes == 656);
            }
            if (actual.tx.MappingCount() > 1) {
                multi++;
            }
            valid++;
        } catch (const std::invalid_argument&) {
            assert(result == slave::Error::Capacity);
            rejected++;
        }
    }
    // 同一未对齐缓冲区由主机写、从站读，再反向验证。
    uint8_t bytes[16]{};
    host::wire::Put64(bytes + 1, 0x123456789abcdef0ULL);
    assert(slave::U64(bytes + 1) == 0x123456789abcdef0ULL);
    slave::Put64(bytes + 1, 0xfedcba9876543210ULL);
    assert(host::wire::U64(bytes + 1) == 0xfedcba9876543210ULL);
    assert(valid > 10 && rejected > 10 && multi > 0);
    std::printf(
        "Host/slave crosscheck PASS: %u layouts, %u rejected, %u multi-PDO\n",
        valid, rejected, multi);
}
