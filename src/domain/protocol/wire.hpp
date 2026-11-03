/* SPDX-License-Identifier: Apache-2.0 */

/* 线协议基础：小端字节序、序号比较、Rx 公共头解码。 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.hpp"
#include "domain/protocol/types.hpp"

namespace ecat {

uint16_t U16(const uint8_t* p);
uint32_t U32(const uint8_t* p);
uint64_t U64(const uint8_t* p);
void Put16(uint8_t* p, uint16_t value);
void Put32(uint8_t* p, uint32_t value);
void Put64(uint8_t* p, uint64_t value);
/* 半空间规则判断序号更新（含 0 与回绕）。 */
bool Newer(uint32_t value, uint32_t previous);
/* 全零检查。 */
bool Zero(const uint8_t* p, size_t size);

/* 解码 RxPDO 公共头并做基本合法性检查。 */
Error DecodeHeader(Bytes image, RxHeader& out);

}  // namespace ecat
