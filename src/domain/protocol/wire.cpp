/* SPDX-License-Identifier: Apache-2.0 */

#include "domain/protocol/wire.hpp"

#include "common/abi.hpp"
#include <pdo_schema.hpp>

namespace ecat {

uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t U32(const uint8_t* p) {
    return U16(p) | uint32_t(U16(p + 2)) << 16;
}
uint64_t U64(const uint8_t* p) {
    return U32(p) | uint64_t(U32(p + 4)) << 32;
}
void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
    Put16(p, uint16_t(v));
    Put16(p + 2, uint16_t(v >> 16));
}
void Put64(uint8_t* p, uint64_t v) {
    Put32(p, uint32_t(v));
    Put32(p + 4, uint32_t(v >> 32));
}
bool Newer(uint32_t n, uint32_t old) {
    const uint32_t d = n - old;
    return d && d < 0x80000000U;
}
bool Zero(const uint8_t* p, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

Error DecodeHeader(Bytes image, RxHeader& out) {
    if (!image.data || image.size < pdo::rx::header::size) {
        return Error::Wire;
    }
    const auto* p = image.data;
    out = {U32(p + pdo::rx::header::session_id),
           U32(p + pdo::rx::header::expected_boot_id),
           U32(p + pdo::rx::header::exchange_sequence),
           p[pdo::rx::header::control_flags],
           p[pdo::rx::header::active_source_mask],
           U16(p + pdo::rx::header::status_request_sequence),
           p[pdo::rx::header::status_target],
           p[pdo::rx::header::status_channel]};
    if (!out.session || (out.sources & ~3U) ||
        (out.flags != kDiscover && out.flags != kArm && out.flags != kStop)) {
        return Error::Wire;
    }
    return Error::None;
}

}  // namespace ecat
