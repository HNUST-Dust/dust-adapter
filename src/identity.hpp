/* SPDX-License-Identifier: Apache-2.0 */

/* 启动身份：直接调用 OTP/NVS 的 Zephyr/HPM API（不再经过端口封装）。
 * 先提交并读回计数，成功后才返回可对外暴露的 boot_id。 */
#pragma once

#include <cstdint>

namespace ecat {

int AllocateBootIdentity(uint64_t& boot);

}  // namespace ecat
