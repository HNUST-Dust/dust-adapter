/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "common/paged_wire.hpp"
#include "domain/pdo/layout.hpp"
namespace ecat {
inline ImageLayout PageCarrierLayout() {
    ImageLayout out;
    out.bytes = dust::paged::kBytes;
    out.first_pdo = 0x1a00;
    out.mapping_count = 1;
    out.mappings[0] = {0x1a00, 8};
    out.count = 8;
    for (unsigned i = 0; i < 8; ++i) {
        out.entries[i] = {dust::paged::kObject, uint8_t(i + 1), 192,
                          uint16_t(i * 24)};
    }
    return out;
}
inline bool PageCarrierMatches(const ImageLayout& in) {
    if (in.bytes != dust::paged::kBytes || in.count != 8 ||
        in.mapping_count != 1 || in.first_pdo != 0x1a00 ||
        in.mappings[0].index != 0x1a00 || in.mappings[0].count != 8) {
        return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
        const auto& a = in.entries[i];
        if (a.index != dust::paged::kObject || a.subindex != i + 1 ||
            a.bits != 192 || a.offset != i * 24) {
            return false;
        }
    }
    return true;
}
inline bool PageProductionProfile(const Configuration& c) {
    if (c.cycle_us != 125 || c.can_tx != std::array<uint8_t, 2>{6, 0} ||
        c.can_rx != std::array<uint8_t, 4>{3, 0, 0, 3} || !c.imu) {
        return false;
    }
    for (unsigned i = 0; i < 6; ++i) {
        if (c.uart[i].tx_bytes || c.uart[i].rx_bytes != (i == 1   ? 128
                                                         : i == 4 ? 32
                                                                  : 0)) {
            return false;
        }
    }
    return true;
}
}  // namespace ecat
