/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstdint>
namespace gateway {
/** @brief 启动 IMU 采集与 USB 上报，返回 IMU 能力位；不可用时返回零。 */
uint32_t StartImuReports();
}  // namespace gateway
