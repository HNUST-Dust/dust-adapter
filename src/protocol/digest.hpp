/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace dust::ecat::detail {
constexpr std::array<uint32_t, 256> DigestTable() {
    std::array<uint32_t, 256> table{};
    for (unsigned n = 0; n < table.size(); ++n) {
        uint32_t crc = n;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
        table[n] = crc;
    }
    return table;
}
inline constexpr auto kDigestTable = DigestTable();
inline uint32_t FastDigest(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ kDigestTable[(crc ^ data[i]) & 0xff];
    return crc ^ 0xffffffffU;
}
} // namespace dust::ecat::detail
