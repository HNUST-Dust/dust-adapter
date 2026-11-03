/* SPDX-License-Identifier: Apache-2.0 */

/* ABI 5.0 常量（冻结）：只允许在此处增加注释，不允许改变数值或语义。 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace ecat {

constexpr uint16_t kAbiMajor = 5;
constexpr uint16_t kAbiMinor = 0;
constexpr uint32_t kCycleUs = 1000;  // 默认交换周期；控制器可独立保持 1 kHz。
constexpr uint32_t kTimeoutUs = 20000;
constexpr uint32_t kProtectionCapability = 1U << 31;
constexpr size_t kMaxImageBytes = 4096;
constexpr size_t kMaxEntries = 1024;

constexpr bool ValidCycle(uint32_t us) {
    return us == 125 || us == 250 || us == 500 || us == 1000;
}

enum class BusinessState : uint32_t {
    Unconfigured = 0,
    Ready = 1,
    Active = 2,
    Protected = 3
};

}  // namespace ecat
