/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file firmware_can_profile.hpp
 * @brief 编译期 CAN 白名单及不可覆盖保护策略的校验。
 */
#pragma once
#include <can_profile_generated.hpp>

namespace dust::can_profile {

/**
 * @brief 判断通道、标识符和标志是否匹配编译期发送白名单。
 */
inline bool AllowsTransmit(const wire::CanFrame& record) {
    for (const auto& address : kTransmitAddresses) {
        if (record.channel == address.channel && record.id == address.id &&
            record.flags == address.flags) {
            return true;
        }
    }
    return false;
}
// 空列表选择 MCU 默认保护，不会删除固件保护帧。

/**
 * @brief 校验租约和保护选择；空 CAN 列表保留 MCU 默认保护。
 */
inline bool AcceptsSafetyRequest(const wire::UsbOutRequest& request) {
    if (!wire::ValidRecords(request, true)) {
        return false;
    }
    const auto timeout = wire::U32(request.data() + 16);
    if (timeout < 1000 || timeout > kCommandTimeoutUs) {
        return false;
    }
    if (!request[10]) {
        return true;
    }
    for (size_t i = 0; i < request[10]; ++i) {
        if (request[32 + i * 16 + 2] != wire::kUartAction) {
            return false;
        }
    }
    return true;
}
}  // namespace dust::can_profile
