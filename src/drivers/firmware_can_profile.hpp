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
inline bool AllowsTransmit(const CanFrame& record) {
    for (const auto& address : kTransmitAddresses) {
        if (record.channel == address.channel && record.id == address.id &&
            record.flags == address.flags) {
            return true;
        }
    }
    return false;
}
}  // namespace dust::can_profile
