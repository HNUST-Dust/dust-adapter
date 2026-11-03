/* SPDX-License-Identifier: Apache-2.0 */

/* PDO 上行编码（ABI 5.0）：公共头/能力/状态/诊断 + CAN/UART/IMU 区域。 */
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/abi.hpp"
#include "domain/protocol/wire.hpp"
#include "domain/protocol/session.hpp"
#include <pdo_schema.hpp>

namespace ecat {
namespace pdo = pdo;

Error Session::Publish(MutableBytes image, uint64_t now,
                       const Feedback& feedback, bool advance_uart) {
    Tick(now, true);
    if (state_ == BusinessState::Unconfigured) {
        return Error::State;
    }
    auto invalid = [&](Error reason) {
        Protect(reason);
        return reason;
    };
    if (!image.data || image.size != layout_.tx.bytes) {
        return invalid(Error::Argument);
    }
    for (unsigned bus = 0; bus < 4; ++bus) {
        const auto& batch = feedback.can[bus];
        if (batch.count > config_.can_rx[bus] ||
            (batch.count && !batch.frames)) {
            return invalid(Error::Capacity);
        }
        for (unsigned n = 0; n < batch.count; ++n) {
            const auto& f = batch.frames[n];
            if (f.id > 0x7ff || f.length > 8 ||
                uint32_t(uint32_t(now) - f.capture_us) >= kTimeoutUs) {
                return invalid(Error::Wire);
            }
        }
    }
    if (feedback.imu_valid &&
        (!config_.imu || !std::isfinite(feedback.temperature) ||
         uint32_t(uint32_t(now) - feedback.imu_capture_us) >= kTimeoutUs)) {
        return invalid(Error::Wire);
    }
    // Tick 由集成层即使在没有 RxPDO 到达时也独立调用。
    // A CAN-only refresh preserves the last UART image byte-for-byte and
    // does not mark another block sent or move the retransmission cursor.
    if (state_ != BusinessState::Active) {
        advance_uart = true;
    }
    if (!advance_uart &&
        (U32(image.data + pdo::tx::header::boot_id) != boot_ ||
         U32(image.data + pdo::tx::header::accepted_session_id) != session_)) {
        return invalid(Error::State);
    }
    auto clear = [&](unsigned from, unsigned to) {
#if defined(CONFIG_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR)
        // Header bytes0..(size-1) and the entire diagnostics block are
        // overwritten on every successful publication. Keep reserved/payload
        // clearing between them.
        from = std::max(from, unsigned(pdo::tx::header::size));
        to = std::min(to, unsigned(layout_.diagnostics));
#endif
        if (from < to) std::fill(image.data + from, image.data + to, uint8_t(0));
    };
    unsigned clear_from = 0;
    if (!advance_uart) {
        for (unsigned port = 0; port < 6; ++port) {
            if (layout_.uart_tx[port] == kAbsent) {
                continue;
            }
            clear(clear_from, layout_.uart_tx[port]);
            clear_from = layout_.uart_tx[port] + pdo::tx::uart::header_size +
                         config_.uart[port].rx_bytes;
        }
    }
    clear(clear_from, image.size);

    auto* p = image.data;
    Put32(p + pdo::tx::header::boot_id, boot_);
    Put32(p + pdo::tx::header::accepted_session_id, session_);
    Put32(p + pdo::tx::header::publish_sequence, ++publish_sequence_);
    Put32(p + pdo::tx::header::slave_now_us, uint32_t(now));
    uint32_t capabilities =
        limits_.can_mask ? 1U | (uint32_t(limits_.can_mask) << 16) : 0;
    for (unsigned i = 0; i < 6; ++i) {
        if ((limits_.uart_tx_mask | limits_.uart_rx_mask) & (1U << i)) {
            capabilities |= 1U << (i < 2 ? i + 1 : i + 2);
        }
    }
    if (limits_.imu) {
        capabilities |= 8;
    }
    if (protection_.installed && protection_.stop &&
        (!(config_.can_tx[0] || config_.can_tx[1]) ||
         protection_.validate_can)) {
        capabilities |= kProtectionCapability;
    }
    Put32(p + pdo::tx::header::capability_mask, capabilities);
    static_assert(uint32_t(Error::UartTimeout) <= UINT8_MAX);
    static_assert(uint32_t(BusinessState::Protected) <= UINT8_MAX);
    p[pdo::tx::header::business_state] = uint8_t(state_);
    p[pdo::tx::header::fault_flags] = uint8_t(fault_);

    can_channel_.Encode(image, layout_, config_, feedback);
    uart_channel_.Encode(image, layout_, config_, now, advance_uart,
                         state_ == BusinessState::Active);

    if (layout_.imu != kAbsent && feedback.imu_valid) {
        auto* out = p + layout_.imu;
        uint32_t temperature;
        static_assert(sizeof(float) == 4, "Wire temperature requires binary32");
        std::memcpy(&temperature, &feedback.temperature, sizeof(temperature));
        Put32(out + pdo::tx::imu::sample_sequence, feedback.imu_sequence);
        Put32(out + pdo::tx::imu::capture_time_us, feedback.imu_capture_us);
        out[pdo::tx::imu::flags] = 1;
        Put32(out + pdo::tx::imu::temperature_c, temperature);
        std::copy(feedback.imu_raw.begin(), feedback.imu_raw.end(),
                  out + pdo::tx::imu::spi_bytes);
    }
    auto* diagnostics = p + layout_.diagnostics;
    Put32(diagnostics + pdo::tx::diagnostics::stop_reason, uint32_t(fault_));
    Put32(diagnostics + pdo::tx::diagnostics::rejected, rejected_);
    Put32(diagnostics + pdo::tx::diagnostics::status_reply_sequence,
          status_reply_);
    Put32(diagnostics + pdo::tx::diagnostics::status_result,
          uint32_t(status_result_));
    for (unsigned i = 0; i < 3; ++i) {
        Put32(diagnostics + pdo::tx::diagnostics::status_value0 + 4 * i,
              status_values_[i]);
    }
    return Error::None;
}

}  // namespace ecat
