/* SPDX-License-Identifier: Apache-2.0 */
// GENERATED from pdo_schema.yaml — DO NOT EDIT.
// Single source of truth for the EtherCAT process image layout.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace dust::pdo {
inline constexpr uint16_t kAbiMajor = 5;
inline constexpr uint16_t kAbiMinor = 0;

// Runtime configuration used by the field walk.
struct Config {
    std::array<uint8_t, 2> can_tx{};   // Rx: chassis/gimbal command slots
    std::array<uint8_t, 4> can_rx{};   // Tx: per-bus feedback slots
    std::array<uint16_t, 6> uart_tx{}; // Rx: host->slave bytes per port
    std::array<uint16_t, 6> uart_rx{}; // Tx: slave->host bytes per port
    bool imu = false;
};

// Fixed production wiring (single source of truth).
inline constexpr uint32_t kProductionCycleUs = 125;
inline constexpr Config ProductionConfig() {
    return {{6, 0}, {0, 0, 3, 3}, {0, 0, 0, 0, 0, 0}, {0, 128, 0, 0, 32, 0}, true};
}

// ---- compile-time offsets for runtime encode/decode ----
namespace rx {
namespace header {
inline constexpr size_t size = 18;
inline constexpr size_t session_id = 0;
inline constexpr size_t expected_boot_id = 4;
inline constexpr size_t exchange_sequence = 8;
inline constexpr size_t control_flags = 12;
inline constexpr size_t active_source_mask = 13;
inline constexpr size_t status_target = 14;
inline constexpr size_t status_channel = 15;
inline constexpr size_t status_request_sequence = 16;
}  // namespace header
namespace can {
inline constexpr size_t header_size = 11;
inline constexpr size_t frame_stride = 12;
inline constexpr size_t source_sequence = 0;
inline constexpr size_t remaining_valid_us = 4;
inline constexpr size_t flags = 8;
inline constexpr size_t frame_count = 9;
inline constexpr size_t source_id = 10;
inline constexpr size_t frame_channel = 0;
inline constexpr size_t frame_length = 1;
inline constexpr size_t frame_id = 2;
inline constexpr size_t frame_data = 4;
}  // namespace can
namespace uart {
inline constexpr size_t header_size = 15;
inline constexpr size_t block_sequence = 0;
inline constexpr size_t peer_ack_sequence = 4;
inline constexpr size_t capture_time_us = 8;
inline constexpr size_t length = 12;
inline constexpr size_t flags = 14;
}  // namespace uart
}  // namespace rx

namespace tx {
namespace header {
inline constexpr size_t size = 22;
inline constexpr size_t boot_id = 0;
inline constexpr size_t accepted_session_id = 4;
inline constexpr size_t publish_sequence = 8;
inline constexpr size_t slave_now_us = 12;
inline constexpr size_t capability_mask = 16;
inline constexpr size_t business_state = 20;
inline constexpr size_t fault_flags = 21;
}  // namespace header
namespace can {
inline constexpr size_t header_size = 9;
inline constexpr size_t frame_stride = 19;
inline constexpr size_t batch_sequence = 0;
inline constexpr size_t cumulative_drops = 4;
inline constexpr size_t frame_count = 8;
inline constexpr size_t frame_id = 0;
inline constexpr size_t frame_bus_sequence = 2;
inline constexpr size_t frame_capture_time_us = 6;
inline constexpr size_t frame_length = 10;
inline constexpr size_t frame_data = 11;
}  // namespace can
namespace uart {
inline constexpr size_t header_size = 15;
inline constexpr size_t block_sequence = 0;
inline constexpr size_t peer_ack_sequence = 4;
inline constexpr size_t capture_time_us = 8;
inline constexpr size_t length = 12;
inline constexpr size_t flags = 14;
}  // namespace uart
namespace imu {
inline constexpr size_t size = 38;
inline constexpr size_t sample_sequence = 0;
inline constexpr size_t capture_time_us = 4;
inline constexpr size_t flags = 8;
inline constexpr size_t temperature_c = 9;
inline constexpr size_t spi_bytes = 13;
}  // namespace imu
namespace diagnostics {
inline constexpr size_t size = 28;
inline constexpr size_t stop_reason = 0;
inline constexpr size_t rejected = 4;
inline constexpr size_t status_reply_sequence = 8;
inline constexpr size_t status_result = 12;
inline constexpr size_t status_value0 = 16;
inline constexpr size_t status_value1 = 20;
inline constexpr size_t status_value2 = 24;
}  // namespace diagnostics
}  // namespace tx

// ---- ordered field walks (layout + CRC + CoE mapping) ----
// RX field walk, in layout order. `emit(object, name, bits)`
template <typename F>
void WalkRx(const Config& c, F&& emit) {
    char name[96];
        emit(0x2000, "header.session_id", 32);
        emit(0x2000, "header.expected_boot_id", 32);
        emit(0x2000, "header.exchange_sequence", 32);
        emit(0x2000, "header.control_flags", 8);
        emit(0x2000, "header.active_source_mask", 8);
        emit(0x2000, "header.status_target", 8);
        emit(0x2000, "header.status_channel", 8);
        emit(0x2000, "header.status_request_sequence", 16);
    if (c.can_tx[0]) {
        emit(0x2010, "can.chassis.source_sequence", 32);
        emit(0x2010, "can.chassis.remaining_valid_us", 32);
        emit(0x2010, "can.chassis.flags", 8);
        emit(0x2010, "can.chassis.frame_count", 8);
        emit(0x2010, "can.chassis.source_id", 8);
        for (unsigned j = 0; j < c.can_tx[0]; ++j) {
            std::snprintf(name, sizeof(name), "can.chassis.frames[%u].channel", j);
            emit(0x2010, name, 8);
            std::snprintf(name, sizeof(name), "can.chassis.frames[%u].length", j);
            emit(0x2010, name, 8);
            std::snprintf(name, sizeof(name), "can.chassis.frames[%u].id", j);
            emit(0x2010, name, 16);
            std::snprintf(name, sizeof(name), "can.chassis.frames[%u].data[0]", j);
            emit(0x2010, name, 64);
        }
    }
    if (c.can_tx[1]) {
        emit(0x2011, "can.gimbal.source_sequence", 32);
        emit(0x2011, "can.gimbal.remaining_valid_us", 32);
        emit(0x2011, "can.gimbal.flags", 8);
        emit(0x2011, "can.gimbal.frame_count", 8);
        emit(0x2011, "can.gimbal.source_id", 8);
        for (unsigned j = 0; j < c.can_tx[1]; ++j) {
            std::snprintf(name, sizeof(name), "can.gimbal.frames[%u].channel", j);
            emit(0x2011, name, 8);
            std::snprintf(name, sizeof(name), "can.gimbal.frames[%u].length", j);
            emit(0x2011, name, 8);
            std::snprintf(name, sizeof(name), "can.gimbal.frames[%u].id", j);
            emit(0x2011, name, 16);
            std::snprintf(name, sizeof(name), "can.gimbal.frames[%u].data[0]", j);
            emit(0x2011, name, 64);
        }
    }
    if (c.uart_tx[0] || c.uart_rx[0]) {
        emit(0x2020, "uart1.block_sequence", 32);
        emit(0x2020, "uart1.peer_ack_sequence", 32);
        emit(0x2020, "uart1.capture_time_us", 32);
        emit(0x2020, "uart1.length", 16);
        emit(0x2020, "uart1.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[0]; n += 30) {
            std::snprintf(name, sizeof(name), "uart1.data[%u]", n);
            emit(0x2020, name, (c.uart_tx[0] - n < 30 ? c.uart_tx[0] - n : 30) * 8);
        }
    }
    if (c.uart_tx[1] || c.uart_rx[1]) {
        emit(0x2021, "uart2.block_sequence", 32);
        emit(0x2021, "uart2.peer_ack_sequence", 32);
        emit(0x2021, "uart2.capture_time_us", 32);
        emit(0x2021, "uart2.length", 16);
        emit(0x2021, "uart2.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[1]; n += 30) {
            std::snprintf(name, sizeof(name), "uart2.data[%u]", n);
            emit(0x2021, name, (c.uart_tx[1] - n < 30 ? c.uart_tx[1] - n : 30) * 8);
        }
    }
    if (c.uart_tx[2] || c.uart_rx[2]) {
        emit(0x2022, "uart3.block_sequence", 32);
        emit(0x2022, "uart3.peer_ack_sequence", 32);
        emit(0x2022, "uart3.capture_time_us", 32);
        emit(0x2022, "uart3.length", 16);
        emit(0x2022, "uart3.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[2]; n += 30) {
            std::snprintf(name, sizeof(name), "uart3.data[%u]", n);
            emit(0x2022, name, (c.uart_tx[2] - n < 30 ? c.uart_tx[2] - n : 30) * 8);
        }
    }
    if (c.uart_tx[3] || c.uart_rx[3]) {
        emit(0x2023, "uart4.block_sequence", 32);
        emit(0x2023, "uart4.peer_ack_sequence", 32);
        emit(0x2023, "uart4.capture_time_us", 32);
        emit(0x2023, "uart4.length", 16);
        emit(0x2023, "uart4.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[3]; n += 30) {
            std::snprintf(name, sizeof(name), "uart4.data[%u]", n);
            emit(0x2023, name, (c.uart_tx[3] - n < 30 ? c.uart_tx[3] - n : 30) * 8);
        }
    }
    if (c.uart_tx[4] || c.uart_rx[4]) {
        emit(0x2024, "uart5.block_sequence", 32);
        emit(0x2024, "uart5.peer_ack_sequence", 32);
        emit(0x2024, "uart5.capture_time_us", 32);
        emit(0x2024, "uart5.length", 16);
        emit(0x2024, "uart5.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[4]; n += 30) {
            std::snprintf(name, sizeof(name), "uart5.data[%u]", n);
            emit(0x2024, name, (c.uart_tx[4] - n < 30 ? c.uart_tx[4] - n : 30) * 8);
        }
    }
    if (c.uart_tx[5] || c.uart_rx[5]) {
        emit(0x2025, "uart6.block_sequence", 32);
        emit(0x2025, "uart6.peer_ack_sequence", 32);
        emit(0x2025, "uart6.capture_time_us", 32);
        emit(0x2025, "uart6.length", 16);
        emit(0x2025, "uart6.flags", 8);
        for (unsigned n = 0; n < c.uart_tx[5]; n += 30) {
            std::snprintf(name, sizeof(name), "uart6.data[%u]", n);
            emit(0x2025, name, (c.uart_tx[5] - n < 30 ? c.uart_tx[5] - n : 30) * 8);
        }
    }
}

// TX field walk, in layout order. `emit(object, name, bits)`
template <typename F>
void WalkTx(const Config& c, F&& emit) {
    char name[96];
        emit(0x3000, "header.boot_id", 32);
        emit(0x3000, "header.accepted_session_id", 32);
        emit(0x3000, "header.publish_sequence", 32);
        emit(0x3000, "header.slave_now_us", 32);
        emit(0x3000, "header.capability_mask", 32);
        emit(0x3000, "header.business_state", 8);
        emit(0x3000, "header.fault_flags", 8);
    if (c.can_rx[0]) {
        emit(0x3010, "can0.batch_sequence", 32);
        emit(0x3010, "can0.cumulative_drops", 32);
        emit(0x3010, "can0.frame_count", 8);
        for (unsigned j = 0; j < c.can_rx[0]; ++j) {
            std::snprintf(name, sizeof(name), "can0.frames[%u].id", j);
            emit(0x3010, name, 16);
            std::snprintf(name, sizeof(name), "can0.frames[%u].bus_sequence", j);
            emit(0x3010, name, 32);
            std::snprintf(name, sizeof(name), "can0.frames[%u].capture_time_us", j);
            emit(0x3010, name, 32);
            std::snprintf(name, sizeof(name), "can0.frames[%u].length", j);
            emit(0x3010, name, 8);
            std::snprintf(name, sizeof(name), "can0.frames[%u].data[0]", j);
            emit(0x3010, name, 64);
        }
    }
    if (c.can_rx[1]) {
        emit(0x3011, "can1.batch_sequence", 32);
        emit(0x3011, "can1.cumulative_drops", 32);
        emit(0x3011, "can1.frame_count", 8);
        for (unsigned j = 0; j < c.can_rx[1]; ++j) {
            std::snprintf(name, sizeof(name), "can1.frames[%u].id", j);
            emit(0x3011, name, 16);
            std::snprintf(name, sizeof(name), "can1.frames[%u].bus_sequence", j);
            emit(0x3011, name, 32);
            std::snprintf(name, sizeof(name), "can1.frames[%u].capture_time_us", j);
            emit(0x3011, name, 32);
            std::snprintf(name, sizeof(name), "can1.frames[%u].length", j);
            emit(0x3011, name, 8);
            std::snprintf(name, sizeof(name), "can1.frames[%u].data[0]", j);
            emit(0x3011, name, 64);
        }
    }
    if (c.can_rx[2]) {
        emit(0x3012, "can2.batch_sequence", 32);
        emit(0x3012, "can2.cumulative_drops", 32);
        emit(0x3012, "can2.frame_count", 8);
        for (unsigned j = 0; j < c.can_rx[2]; ++j) {
            std::snprintf(name, sizeof(name), "can2.frames[%u].id", j);
            emit(0x3012, name, 16);
            std::snprintf(name, sizeof(name), "can2.frames[%u].bus_sequence", j);
            emit(0x3012, name, 32);
            std::snprintf(name, sizeof(name), "can2.frames[%u].capture_time_us", j);
            emit(0x3012, name, 32);
            std::snprintf(name, sizeof(name), "can2.frames[%u].length", j);
            emit(0x3012, name, 8);
            std::snprintf(name, sizeof(name), "can2.frames[%u].data[0]", j);
            emit(0x3012, name, 64);
        }
    }
    if (c.can_rx[3]) {
        emit(0x3013, "can3.batch_sequence", 32);
        emit(0x3013, "can3.cumulative_drops", 32);
        emit(0x3013, "can3.frame_count", 8);
        for (unsigned j = 0; j < c.can_rx[3]; ++j) {
            std::snprintf(name, sizeof(name), "can3.frames[%u].id", j);
            emit(0x3013, name, 16);
            std::snprintf(name, sizeof(name), "can3.frames[%u].bus_sequence", j);
            emit(0x3013, name, 32);
            std::snprintf(name, sizeof(name), "can3.frames[%u].capture_time_us", j);
            emit(0x3013, name, 32);
            std::snprintf(name, sizeof(name), "can3.frames[%u].length", j);
            emit(0x3013, name, 8);
            std::snprintf(name, sizeof(name), "can3.frames[%u].data[0]", j);
            emit(0x3013, name, 64);
        }
    }
    if (c.uart_tx[0] || c.uart_rx[0]) {
        emit(0x3020, "uart1.block_sequence", 32);
        emit(0x3020, "uart1.peer_ack_sequence", 32);
        emit(0x3020, "uart1.capture_time_us", 32);
        emit(0x3020, "uart1.length", 16);
        emit(0x3020, "uart1.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[0]; n += 30) {
            std::snprintf(name, sizeof(name), "uart1.data[%u]", n);
            emit(0x3020, name, (c.uart_rx[0] - n < 30 ? c.uart_rx[0] - n : 30) * 8);
        }
    }
    if (c.uart_tx[1] || c.uart_rx[1]) {
        emit(0x3021, "uart2.block_sequence", 32);
        emit(0x3021, "uart2.peer_ack_sequence", 32);
        emit(0x3021, "uart2.capture_time_us", 32);
        emit(0x3021, "uart2.length", 16);
        emit(0x3021, "uart2.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[1]; n += 30) {
            std::snprintf(name, sizeof(name), "uart2.data[%u]", n);
            emit(0x3021, name, (c.uart_rx[1] - n < 30 ? c.uart_rx[1] - n : 30) * 8);
        }
    }
    if (c.uart_tx[2] || c.uart_rx[2]) {
        emit(0x3022, "uart3.block_sequence", 32);
        emit(0x3022, "uart3.peer_ack_sequence", 32);
        emit(0x3022, "uart3.capture_time_us", 32);
        emit(0x3022, "uart3.length", 16);
        emit(0x3022, "uart3.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[2]; n += 30) {
            std::snprintf(name, sizeof(name), "uart3.data[%u]", n);
            emit(0x3022, name, (c.uart_rx[2] - n < 30 ? c.uart_rx[2] - n : 30) * 8);
        }
    }
    if (c.uart_tx[3] || c.uart_rx[3]) {
        emit(0x3023, "uart4.block_sequence", 32);
        emit(0x3023, "uart4.peer_ack_sequence", 32);
        emit(0x3023, "uart4.capture_time_us", 32);
        emit(0x3023, "uart4.length", 16);
        emit(0x3023, "uart4.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[3]; n += 30) {
            std::snprintf(name, sizeof(name), "uart4.data[%u]", n);
            emit(0x3023, name, (c.uart_rx[3] - n < 30 ? c.uart_rx[3] - n : 30) * 8);
        }
    }
    if (c.uart_tx[4] || c.uart_rx[4]) {
        emit(0x3024, "uart5.block_sequence", 32);
        emit(0x3024, "uart5.peer_ack_sequence", 32);
        emit(0x3024, "uart5.capture_time_us", 32);
        emit(0x3024, "uart5.length", 16);
        emit(0x3024, "uart5.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[4]; n += 30) {
            std::snprintf(name, sizeof(name), "uart5.data[%u]", n);
            emit(0x3024, name, (c.uart_rx[4] - n < 30 ? c.uart_rx[4] - n : 30) * 8);
        }
    }
    if (c.uart_tx[5] || c.uart_rx[5]) {
        emit(0x3025, "uart6.block_sequence", 32);
        emit(0x3025, "uart6.peer_ack_sequence", 32);
        emit(0x3025, "uart6.capture_time_us", 32);
        emit(0x3025, "uart6.length", 16);
        emit(0x3025, "uart6.flags", 8);
        for (unsigned n = 0; n < c.uart_rx[5]; n += 30) {
            std::snprintf(name, sizeof(name), "uart6.data[%u]", n);
            emit(0x3025, name, (c.uart_rx[5] - n < 30 ? c.uart_rx[5] - n : 30) * 8);
        }
    }
    if (c.imu) {
        emit(0x3030, "imu.sample_sequence", 32);
        emit(0x3030, "imu.capture_time_us", 32);
        emit(0x3030, "imu.flags", 8);
        emit(0x3030, "imu.temperature_c", 32);
        emit(0x3030, "imu.spi_bytes[0]", 200);
    }
        emit(0x3040, "diagnostics.stop_reason", 32);
        emit(0x3040, "diagnostics.rejected", 32);
        emit(0x3040, "diagnostics.status_reply_sequence", 32);
        emit(0x3040, "diagnostics.status_result", 32);
        emit(0x3040, "diagnostics.status_value0", 32);
        emit(0x3040, "diagnostics.status_value1", 32);
        emit(0x3040, "diagnostics.status_value2", 32);
}

}  // namespace dust::pdo

