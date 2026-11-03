/* SPDX-License-Identifier: Apache-2.0 */

/* 配置、布局与 PDO 影像模型：纯 C++17，与 SSC/Zephyr/外设解耦。
 * 对象索引/字段常量唯一来源是构建期生成的 pdo_schema.hpp。 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/abi.hpp"
#include "common/types.hpp"

namespace ecat {

struct UartCapacity {
    uint16_t tx_bytes = 0;  // 主机 -> 从站。
    uint16_t rx_bytes = 0;
};

struct Configuration {
    uint32_t signature = 0;
    uint32_t cycle_us = kCycleUs;
    uint32_t watchdog_us = kTimeoutUs;
    std::array<uint8_t, 2> can_tx{};
    std::array<uint8_t, 4> can_rx{};
    std::array<UartCapacity, 6> uart{};
    bool imu = false;
};

/* 资源由后续 ESC/板级集成提供，核心不猜测芯片 RAM 或虚构已安装的保护。 */
struct Limits {
    uint16_t rx_bytes = 0;
    uint16_t tx_bytes = 0;
    uint32_t process_ram_bytes = 0;  // 已扣邮箱/保留区，可用于三缓冲的预算。
    uint8_t can_mask = 0;
    uint8_t uart_tx_mask = 0;
    uint8_t uart_rx_mask = 0;
    bool imu = false;
};

struct Entry {
    uint16_t index = 0;
    uint8_t subindex = 0;
    uint8_t bits = 0;
    uint16_t offset = 0;
};

struct Mapping {
    uint16_t index = 0;
    uint16_t count = 0;
};

struct ImageLayout {
    uint16_t bytes = 0;
    uint16_t count = 0;
    uint16_t first_pdo = 0;
    uint16_t mapping_count = 0;
    std::array<Mapping, 5> mappings{};
    std::array<Entry, kMaxEntries> entries{};
    uint16_t MappingCount() const;
};

struct Layout {
    ImageLayout rx;
    ImageLayout tx;
    uint32_t signature = 0;
    std::array<uint16_t, 2> source{{kAbsent, kAbsent}};
    std::array<uint16_t, 4> feedback{{kAbsent, kAbsent, kAbsent, kAbsent}};
    std::array<uint16_t, 6> uart_rx{
        {kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent}};
    std::array<uint16_t, 6> uart_tx{
        {kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent}};
    uint16_t imu = kAbsent;
    uint16_t diagnostics = 0;
};

/* 由配置与 Limits 生成候选布局；不执行任何硬件配置。 */
Error BuildLayout(const Configuration& config, const Limits& limits,
                  Layout& out);

/* staging 接收 0x2100:1..22，顺序任意；提交必须同时匹配实际 PDO/SM 映射。 */
class ConfigurationTransaction {
   public:
    Error Write(uint8_t subindex, uint32_t value);
    Error Commit(const Limits& limits, const ImageLayout& rx,
                 const ImageLayout& tx, Layout& layout) const;
    const Configuration& candidate() const {
        return candidate_;
    }
    void Clear();

   private:
    Configuration candidate_{};
    uint32_t present_ = 0;
};

}  // namespace ecat
