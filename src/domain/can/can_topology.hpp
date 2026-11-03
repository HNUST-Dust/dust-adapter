/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 编译期拓扑：白名单、接收过滤器、保护帧与命令租约上限。
 * 由 board 从生成的 can_profile 数据构造；domain 不 include 生成头。 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.hpp"

namespace ecat {

struct CanTxAddress {
    uint8_t bus = 0;
    uint32_t id = 0;
    uint8_t flags = 0;
};

struct CanTopology {
    const CanTxAddress* tx_addresses = nullptr;
    size_t tx_count = 0;
    const CanFilterConfig* filters = nullptr;
    size_t filter_count = 0;
    const CanFrame* protection_frames = nullptr;
    size_t protection_count = 0;
    uint32_t command_timeout_us = 0;
    uint32_t protection_signature = 0;
    bool protect_on_boot = false;

    bool AllowsTransmit(const CanFrame& frame) const {
        for (size_t i = 0; i < tx_count; ++i) {
            const auto& address = tx_addresses[i];
            if (address.bus == frame.bus && address.id == frame.id &&
                address.flags == 0) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace ecat
