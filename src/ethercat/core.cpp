/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <cmath>
#include <cstring>

#include "protocol.hpp"
#include "digest.hpp"
#include "pdo_schema.hpp"


namespace dust::ecat {
namespace pdo = dust::pdo;
namespace {
uint32_t Digest(Bytes bytes) {
#if defined(DUST_TEST_REFERENCE_DIGEST)
    uint32_t crc = 0xffffffffU;
    for (size_t n = 0; n < bytes.size; ++n) {
        crc ^= bytes.data[n];
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
        }
    }
    return crc ^ 0xffffffffU;
#else
    return detail::FastDigest(bytes.data, bytes.size);
#endif
}
bool Zero(const uint8_t* p, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}
}  // namespace
Core::Core(uint32_t boot_id, Protection protection)
    : protection_(protection), boot_(boot_id) {}
Core::~Core() {
    if (state_ == BusinessState::Active) {
        Protect(Error::State);
    }
}
Error Core::Configure(const ConfigurationTransaction& t, const Limits& limits,
                      const ImageLayout& rx, const ImageLayout& tx) {
    if (!boot_) {
        return Error::Argument;
    }
    if (state_ != BusinessState::Unconfigured) {
        return Error::State;
    }
    const Error result = t.Commit(limits, rx, tx, layout_);
    if (result != Error::None) {
        return result;
    }
    config_ = t.candidate();
    limits_ = limits;
    state_ = BusinessState::Ready;
    return Error::None;
}
void Core::Protect(Error reason, const char* file, unsigned line) {
    if (state_ == BusinessState::Protected) {
        return;
    }
    (void)file; (void)line;
    fault_ = reason == Error::None ? Error::Protection : reason;
    state_ = BusinessState::Protected;
    have_uart_offer_.fill(false);
    // 回调在当前执行上下文同步调用；集成层必须使本地保护实际生效。
    if (protection_.stop) {
        protection_.stop(protection_.context, fault_);
    }
}
void Core::Tick(uint64_t now, bool operational, const char* file, unsigned line) {
    (void)file; (void)line;
    if (state_ != BusinessState::Active) {
        return;
    }
    if (!operational || now < exchange_at_) {
        Protect(Error::Transport);
        return;
    }
    if (now - exchange_at_ >= config_.watchdog_us) {
        Protect(Error::Lease);
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((source_mask_ & (1U << i)) && now >= sources_[i].deadline) {
            Protect(Error::Lease);
            return;
        }
    }
    for (const auto& window : windows_) {
        for (unsigned i = 0; i < window.count; ++i) {
            const auto& b = window.blocks[(window.head + i) % 4];
            if (now < b.queued_at || now - b.queued_at >= kTimeoutUs ||
                uint32_t(uint32_t(now) - b.capture) >= kTimeoutUs ||
                (b.sent && now - b.sent_at >= kTimeoutUs)) {
                Protect(Error::UartTimeout);
                return;
            }
        }
    }
}
bool Core::UartWindow::CanAck(uint32_t value) const {
    if (have_ack) {
        if (value == last_ack) {
            return true;
        }
        if (!Newer(value, last_ack)) {
            return Newer(last_ack, value);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto& b = blocks[(head + i) % 4];
        if (!b.sent) {
            return false;
        }
        if (b.sequence == value) {
            return true;
        }
    }
    return false;
}
void Core::UartWindow::Ack(uint32_t value) {
    if (have_ack && !Newer(value, last_ack)) {
        return;
    }
    while (count) {
        const uint32_t sequence = blocks[head].sequence;
        head = (head + 1) % 4;
        count--;
        if (sequence == value) {
            break;
        }
    }
    last_ack = value;
    have_ack = true;
    cursor = 0;
}
Error Core::Process(Bytes image, uint64_t now, bool operational,
                    Actions& actions) {
    actions = Actions{};
    auto reject = [&](Error reason) {
        actions = Actions{};
        rejected_++;
        Protect(reason);
        return reason;
    };
    if (state_ == BusinessState::Unconfigured) {
        return Error::State;
    }
    Tick(now, operational);
    RxHeader h;
    Error error = DecodeHeader(image, h);
    if (error != Error::None || image.size != layout_.rx.bytes) {
        return reject(Error::Wire);
    }
    if (h.flags == kStop) {
        if (!Zero(image.data + 28, image.size - 28)) {
            return reject(Error::Wire);
        }
        if (state_ == BusinessState::Active &&
            (h.session != session_ || h.boot != boot_)) {
            return reject(Error::Session);
        }
        Protect(Error::State);
        return Error::None;
    }
    if (state_ == BusinessState::Protected) {
        return fault_;
    }
    if (!operational) {
        return reject(Error::Transport);
    }
    if (h.flags != kDiscover && h.boot != boot_) {
        return reject(Error::Session);
    }
    if (h.flags == kDiscover && h.boot && h.boot != boot_) {
        return reject(Error::Session);
    }
    if (state_ == BusinessState::Active &&
        (h.session != session_ || h.sources != source_mask_ ||
         h.flags != kArm)) {
        return reject(Error::Session);
    }
    const uint32_t digest = Digest(image);
    if (have_exchange_ && exchange_session_ == h.session) {
        if (h.exchange == exchange_) {
            if (digest != exchange_digest_) {
                return reject(Error::Sequence);
            }
            return Error::None;  // 重复 PDO 不续看门狗，也不重新派发。
        }
        if (!Newer(h.exchange, exchange_)) {
            return reject(Error::Sequence);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((h.sources & (1U << i)) && !config_.can_tx[i]) {
            return reject(Error::Capacity);
        }
    }
    if (h.flags == kArm && (!protection_.installed || !protection_.stop ||
                            ((config_.can_tx[0] || config_.can_tx[1]) &&
                             !protection_.validate_can))) {
        return reject(Error::Protection);
    }

    auto next_sources = sources_;
    const bool arming = h.flags == kArm && state_ == BusinessState::Ready;
    if (arming) {
        for (auto& source : next_sources) {
            source = Source{};
            source.deadline = now + kTimeoutUs;
        }
    }
    for (unsigned source = 0; source < 2; ++source) {
        if (layout_.source[source] == kAbsent) {
            continue;
        }
        const auto* p = image.data + layout_.source[source];
        const unsigned flags = p[pdo::rx::can::flags];
        const unsigned count = p[pdo::rx::can::frame_count];
        if (flags > 1 || count > config_.can_tx[source]) {
            return reject(Error::Wire);
        }
        if (!flags) {
            if (!Zero(p, pdo::rx::can::header_size +
                           pdo::rx::can::frame_stride * config_.can_tx[source])) {
                return reject(Error::Wire);
            }
            continue;
        }
        if (!(h.sources & (1U << source)) ||
            p[pdo::rx::can::source_id] != source ||
            !U32(p + pdo::rx::can::remaining_valid_us) ||
            U32(p + pdo::rx::can::remaining_valid_us) > kTimeoutUs) {
            return reject(Error::Lease);
        }
        CanBatch& batch = actions.can[source];
        batch.sequence = U32(p + pdo::rx::can::source_sequence);
        batch.count = uint8_t(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto* f = p + pdo::rx::can::header_size +
                            pdo::rx::can::frame_stride * i;
            CanFrame& frame = batch.frames[i];
            frame.bus = f[pdo::rx::can::frame_channel];
            frame.length = f[pdo::rx::can::frame_length];
            frame.id = U16(f + pdo::rx::can::frame_id);
            if (frame.bus >= 4 || !(limits_.can_mask & (1U << frame.bus)) ||
                frame.length > 8 || frame.id > 0x7ff) {
                return reject(Error::Wire);
            }
            std::copy_n(f + pdo::rx::can::frame_data, 8, frame.data.begin());
            if (!protection_.validate_can ||
                !protection_.validate_can(protection_.context, source, frame)) {
                return reject(Error::Protection);
            }
        }
        if (!Zero(p + pdo::rx::can::header_size +
                      count * pdo::rx::can::frame_stride,
                  (config_.can_tx[source] - count) * pdo::rx::can::frame_stride)) {
            return reject(Error::Wire);
        }
        const uint32_t batch_digest =
            Digest({p + pdo::rx::can::flags,
                    size_t(pdo::rx::can::header_size - pdo::rx::can::flags +
                           pdo::rx::can::frame_stride *
                               config_.can_tx[source])});
        auto& previous = next_sources[source];
        if (previous.present && batch.sequence == previous.sequence) {
            if (previous.digest != batch_digest) {
                return reject(Error::Sequence);
            }
        } else {
            if (previous.present && !Newer(batch.sequence, previous.sequence)) {
                return reject(Error::Sequence);
            }
            previous = {true, batch.sequence,
                        now + U32(p + pdo::rx::can::remaining_valid_us),
                        batch_digest};
            batch.valid = h.flags == kArm;
            batch.deadline_us = previous.deadline;
        }
    }
    // 在所有区验证完毕前，不更新任何累计 ACK 或业务授权。
    for (unsigned i = 0; i < 6; ++i) {
        if (layout_.uart_rx[i] == kAbsent) {
            continue;
        }
        const auto* p = image.data + layout_.uart_rx[i];
        const unsigned length = U16(p + pdo::rx::uart::length),
                       flags = p[pdo::rx::uart::flags];
        if ((flags & ~7U) || length > config_.uart[i].tx_bytes ||
            (!(flags & kDataValid) && length)) {
            return reject(Error::Wire);
        }
        if ((flags & kAckValid) &&
            !windows_[i].CanAck(U32(p + pdo::rx::uart::peer_ack_sequence))) {
            return reject(Error::Sequence);
        }
        if (!(flags & kDataValid)) {
            continue;
        }
        if (!length || h.flags != kArm) {
            return reject(Error::Wire);
        }
        const uint32_t sequence = U32(p + pdo::rx::uart::block_sequence);
        const uint32_t expected = have_uart_ack_[i] ? uart_ack_[i] + 1U : 1U;
        if (sequence == expected) {
            actions.uart[i] = {
                true, sequence, bool(flags & kDiscontinuity),
                {p + pdo::rx::uart::header_size, length}};
        }
    }
    if (arming) {
        session_ = h.session;
        source_mask_ = h.sources;
        state_ = BusinessState::Active;
    }
    if (h.flags == kArm) {
        sources_ = next_sources;
    }
    for (unsigned i = 0; i < 6; ++i) {
        have_uart_offer_[i] = actions.uart[i].valid;
        uart_offered_[i] = actions.uart[i].sequence;
        if (layout_.uart_rx[i] != kAbsent) {
            const auto* p = image.data + layout_.uart_rx[i];
            if (p[pdo::rx::uart::flags] & kAckValid) {
                windows_[i].Ack(U32(p + pdo::rx::uart::peer_ack_sequence));
            }
        }
    }
    if (h.status_sequence && h.status_sequence != status_request_) {
        status_request_ = h.status_sequence;
        actions.status_sequence = h.status_sequence;
        actions.status_target = h.status_target;
        actions.status_channel = h.status_channel;
    }
    exchange_ = h.exchange;
    exchange_digest_ = digest;
    exchange_session_ = h.session;
    have_exchange_ = true;
    exchange_at_ = now;
    return Error::None;
}
bool Core::CommitUart(unsigned port, uint32_t sequence) {
    if (state_ != BusinessState::Active || port >= 6 ||
        !have_uart_offer_[port] || uart_offered_[port] != sequence) {
        return false;
    }
    uart_ack_[port] = sequence;
    have_uart_ack_[port] = true;
    have_uart_offer_[port] = false;
    return true;
}
bool Core::QueueUart(unsigned port, Bytes bytes, uint32_t capture, uint64_t now,
                     bool discontinuity) {
    Tick(now, true);
    if (state_ != BusinessState::Active || port >= 6 || !bytes.data ||
        !bytes.size || bytes.size > config_.uart[port].rx_bytes ||
        uint32_t(uint32_t(now) - capture) >= kTimeoutUs) {
        return false;
    }
    auto& window = windows_[port];
    if (window.count == 4) {
        return false;
    }
    auto& b = window.blocks[(window.head + window.count) % 4];
    b = Block{};
    b.sequence = window.next++;
    b.length = uint16_t(bytes.size);
    b.capture = capture;
    b.queued_at = now;
    b.discontinuity = discontinuity;
    std::copy_n(bytes.data, bytes.size, b.data.begin());
    window.count++;
    return true;
}
uint32_t Core::UartAck(unsigned port) const {
    return port < 6 ? uart_ack_[port] : 0;
}
bool Core::UartAckValid(unsigned port) const {
    return port < 6 && have_uart_ack_[port];
}
void Core::CompleteStatus(uint32_t sequence, int32_t result,
                          std::array<uint32_t, 3> values) {
    if (sequence && sequence == status_request_) {
        status_reply_ = sequence;
        status_result_ = result;
        status_values_ = values;
    }
}
Error Core::Publish(MutableBytes image, uint64_t now,
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
#if defined(CONFIG_DUST_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR)
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
    for (unsigned bus = 0; bus < 4; ++bus) {
        if (layout_.feedback[bus] == kAbsent) {
            continue;
        }
        auto* out = p + layout_.feedback[bus];
        const auto& batch = feedback.can[bus];
        Put32(out + pdo::tx::can::batch_sequence, batch.sequence);
        Put32(out + pdo::tx::can::cumulative_drops, batch.drops);
        out[pdo::tx::can::frame_count] = uint8_t(batch.count);
        for (unsigned n = 0; n < batch.count; ++n) {
            auto* f = out + pdo::tx::can::header_size +
                      pdo::tx::can::frame_stride * n;
            const auto& event = batch.frames[n];
            Put16(f + pdo::tx::can::frame_id, event.id);
            Put32(f + pdo::tx::can::frame_bus_sequence, event.sequence);
            Put32(f + pdo::tx::can::frame_capture_time_us, event.capture_us);
            f[pdo::tx::can::frame_length] = event.length;
            std::copy(event.data.begin(), event.data.end(),
                      f + pdo::tx::can::frame_data);
        }
    }
    for (unsigned port = 0; port < 6; ++port) {
        if (!advance_uart || layout_.uart_tx[port] == kAbsent) {
            continue;
        }
        auto* out = p + layout_.uart_tx[port];
        uint8_t flags = 0;
        if (have_uart_ack_[port]) {
            flags |= kAckValid;
            Put32(out + pdo::tx::uart::peer_ack_sequence, uart_ack_[port]);
        }
        auto& w = windows_[port];
        if (state_ == BusinessState::Active && w.count) {
            auto& b = w.blocks[(w.head + w.cursor) % 4];
            if (!b.sent) {
                b.sent = true;
                b.sent_at = now;
            }
            Put32(out + pdo::tx::uart::block_sequence, b.sequence);
            Put32(out + pdo::tx::uart::capture_time_us, b.capture);
            Put16(out + pdo::tx::uart::length, b.length);
            flags |= kDataValid | (b.discontinuity ? kDiscontinuity : 0);
            std::copy_n(b.data.begin(), b.length, out + pdo::tx::uart::header_size);
            w.cursor = (w.cursor + 1) % w.count;
        }
        out[pdo::tx::uart::flags] = flags;
    }
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
}  // namespace dust::ecat
