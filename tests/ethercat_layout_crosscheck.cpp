/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdio>
#include <random>
#include <stdexcept>

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
        slave::Layout actual;
        const auto result = slave::BuildLayout(
            c, {4096, 4096, 24576, 15, 63, 63, true}, actual);
        host::PdoConfig config;
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
