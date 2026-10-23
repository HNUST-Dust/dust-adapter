/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace platform {
/** @brief 读取完整 UID，生成大写十六进制序列号和确定性的 16 位 PID。 */
int ReadUsbIdentity(uint16_t& pid, char* serial, size_t capacity);
}  // namespace platform
