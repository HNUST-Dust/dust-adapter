/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <cmath>
#include <cstring>

#include "protocol.hpp"
#include "digest.hpp"

#if defined(CONFIG_DUST_ECAT_DC_SYNC)
// RAM-only first protection snapshot. File points into the matching ELF rodata.
extern "C" {
struct DustFirstProtection {
    uint32_t version, valid, reason, state, file, line;
    uint32_t now_lo, now_hi, exchange_lo, exchange_hi, operational, watchdog_us, tick_file, tick_line;
};
volatile DustFirstProtection dust_first_protection = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}
static uint64_t protection_tick_now;
static uint64_t protection_tick_exchange;
static bool protection_tick_operational;
static const char* protection_tick_file;
static unsigned protection_tick_line;
#endif

namespace dust::ecat {
namespace {
uint32_t Digest(Bytes bytes) {
#if defined(CONFIG_DUST_ECAT_REFERENCE_DIGEST)
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
Core::Core(uint64_t boot_id, Protection protection)
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
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    if (!dust_first_protection.valid) {
        dust_first_protection.reason = uint32_t(reason);
        dust_first_protection.state = uint32_t(state_);
        dust_first_protection.file = uint32_t(reinterpret_cast<uintptr_t>(file));
        dust_first_protection.line = line;
        dust_first_protection.now_lo = uint32_t(protection_tick_now);
        dust_first_protection.now_hi = uint32_t(protection_tick_now >> 32);
        dust_first_protection.exchange_lo = uint32_t(protection_tick_exchange);
        dust_first_protection.exchange_hi = uint32_t(protection_tick_exchange >> 32);
        dust_first_protection.operational = protection_tick_operational;
        dust_first_protection.watchdog_us = config_.watchdog_us;
        dust_first_protection.tick_file = uint32_t(reinterpret_cast<uintptr_t>(protection_tick_file));
        dust_first_protection.tick_line = protection_tick_line;
        dust_first_protection.valid = 1;
    }
#else
    (void)file; (void)line;
#endif
    fault_ = reason == Error::None ? Error::Protection : reason;
    state_ = BusinessState::Protected;
    have_uart_offer_.fill(false);
    // 回调在当前执行上下文同步调用；集成层必须使本地保护实际生效。
    if (protection_.stop) {
        protection_.stop(protection_.context, fault_);
    }
}
void Core::Tick(uint64_t now, bool operational, const char* file, unsigned line) {
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    protection_tick_file = file;
    protection_tick_line = line;
    protection_tick_now = now;
    protection_tick_exchange = exchange_at_;
    protection_tick_operational = operational;
#else
    (void)file; (void)line;
#endif
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
    if (error != Error::None || image.size != layout_.rx.bytes ||
        U32(image.data + 40) != config_.cycle_us) {
        return reject(Error::Wire);
    }
    if (h.signature != layout_.signature) {
        return reject(Error::Signature);
    }
    if (h.flags == kStop) {
        if (!Zero(image.data + 64, image.size - 64)) {
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
        const uint16_t flags = U16(p + 8);
        const unsigned count = p[10];
        if (flags > 1 || count > config_.can_tx[source] || U32(p + 12)) {
            return reject(Error::Wire);
        }
        if (!flags) {
            if (!Zero(p, 16 + 16 * config_.can_tx[source])) {
                return reject(Error::Wire);
            }
            continue;
        }
        if (!(h.sources & (1U << source)) || p[11] != source || !U32(p + 4) ||
            U32(p + 4) > kTimeoutUs) {
            return reject(Error::Lease);
        }
        CanBatch& batch = actions.can[source];
        batch.sequence = U32(p);
        batch.count = uint8_t(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto* f = p + 16 + 16 * i;
            CanFrame& frame = batch.frames[i];
            frame.bus = f[0];
            frame.length = f[1];
            frame.id = U32(f + 4);
            if (frame.bus >= 4 || !(limits_.can_mask & (1U << frame.bus)) ||
                frame.length > 8 || frame.id > 0x7ff || f[2] || f[3]) {
                return reject(Error::Wire);
            }
            std::copy_n(f + 8, 8, frame.data.begin());
            if (!protection_.validate_can ||
                !protection_.validate_can(protection_.context, source, frame)) {
                return reject(Error::Protection);
            }
        }
        if (!Zero(p + 16 + count * 16, (config_.can_tx[source] - count) * 16)) {
            return reject(Error::Wire);
        }
        const uint32_t batch_digest =
            Digest({p + 8, size_t(8 + 16 * config_.can_tx[source])});
        auto& previous = next_sources[source];
        if (previous.present && batch.sequence == previous.sequence) {
            if (previous.digest != batch_digest) {
                return reject(Error::Sequence);
            }
        } else {
            if (previous.present && !Newer(batch.sequence, previous.sequence)) {
                return reject(Error::Sequence);
            }
            previous = {true, batch.sequence, now + U32(p + 4), batch_digest};
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
        const unsigned length = U16(p + 12), flags = U16(p + 14);
        if ((flags & ~7U) || length > config_.uart[i].tx_bytes ||
            (!(flags & kDataValid) && length)) {
            return reject(Error::Wire);
        }
        if ((flags & kAckValid) && !windows_[i].CanAck(U32(p + 4))) {
            return reject(Error::Sequence);
        }
        if (!(flags & kDataValid)) {
            continue;
        }
        if (!length || h.flags != kArm) {
            return reject(Error::Wire);
        }
        const uint32_t sequence = U32(p);
        const uint32_t expected = have_uart_ack_[i] ? uart_ack_[i] + 1U : 1U;
        if (sequence == expected) {
            actions.uart[i] = {
                true, sequence, bool(flags & kDiscontinuity), {p + 16, length}};
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
            if (U16(p + 14) & kAckValid) {
                windows_[i].Ack(U32(p + 4));
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
uint32_t Core::AcceptedCan(unsigned source) const {
    return source < 2 ? sources_[source].sequence : 0;
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
        (U32(image.data) != kMagic || U64(image.data + 8) != boot_ ||
         U64(image.data + 16) != session_ ||
         U32(image.data + 32) != layout_.signature)) {
        return invalid(Error::State);
    }
    auto clear = [&](unsigned from, unsigned to) {
#if defined(CONFIG_DUST_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR)
        // Header bytes0..59 and the entire diagnostics block are overwritten
        // on every successful publication. Keep reserved/payload clearing.
        from = std::max(from, 60U);
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
            clear_from =
                layout_.uart_tx[port] + 16 + config_.uart[port].rx_bytes;
        }
    }
    clear(clear_from, image.size);
    auto* p = image.data;
    Put32(p, kMagic);
    Put16(p + 4, kAbiMajor);
    Put16(p + 6, kAbiMinor);
    Put64(p + 8, boot_);
    Put64(p + 16, session_);
    Put32(p + 24, ++publish_sequence_);
    Put32(p + 28, uint32_t(now));
    Put32(p + 32, layout_.signature);
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
    Put32(p + 36, capabilities);
    Put32(p + 40, uint32_t(state_));
    Put32(p + 44, uint32_t(fault_));
    Put32(p + 48, AcceptedCan(0));
    Put32(p + 52, AcceptedCan(1));
    Put32(p + 56, status_reply_);
    for (unsigned bus = 0; bus < 4; ++bus) {
        if (layout_.feedback[bus] == kAbsent) {
            continue;
        }
        auto* out = p + layout_.feedback[bus];
        const auto& batch = feedback.can[bus];
        Put32(out, batch.sequence);
        Put32(out + 4, batch.drops);
        Put16(out + 8, batch.count);
        for (unsigned n = 0; n < batch.count; ++n) {
            auto* f = out + 16 + 24 * n;
            const auto& event = batch.frames[n];
            Put32(f, event.id);
            Put32(f + 4, event.sequence);
            Put32(f + 8, event.capture_us);
            f[12] = event.length;
            std::copy(event.data.begin(), event.data.end(), f + 16);
        }
    }
    for (unsigned port = 0; port < 6; ++port) {
        if (!advance_uart || layout_.uart_tx[port] == kAbsent) {
            continue;
        }
        auto* out = p + layout_.uart_tx[port];
        uint16_t flags = 0;
        if (have_uart_ack_[port]) {
            flags |= kAckValid;
            Put32(out + 4, uart_ack_[port]);
        }
        auto& w = windows_[port];
        if (state_ == BusinessState::Active && w.count) {
            auto& b = w.blocks[(w.head + w.cursor) % 4];
            if (!b.sent) {
                b.sent = true;
                b.sent_at = now;
            }
            Put32(out, b.sequence);
            Put32(out + 8, b.capture);
            Put16(out + 12, b.length);
            flags |= kDataValid | (b.discontinuity ? kDiscontinuity : 0);
            std::copy_n(b.data.begin(), b.length, out + 16);
            w.cursor = (w.cursor + 1) % w.count;
        }
        Put16(out + 14, flags);
    }
    if (layout_.imu != kAbsent && feedback.imu_valid) {
        auto* out = p + layout_.imu;
        uint32_t temperature;
        static_assert(sizeof(float) == 4, "Wire temperature requires binary32");
        std::memcpy(&temperature, &feedback.temperature, sizeof(temperature));
        Put32(out, feedback.imu_sequence);
        Put32(out + 4, feedback.imu_capture_us);
        Put32(out + 8, 1);
        Put32(out + 12, temperature);
        std::copy(feedback.imu_raw.begin(), feedback.imu_raw.end(), out + 16);
    }
    auto* diagnostics = p + layout_.diagnostics;
    std::copy(feedback.diagnostics.begin(), feedback.diagnostics.end(),
              diagnostics);
    Put32(diagnostics, uint32_t(fault_));
    Put32(diagnostics + 4, rejected_);
    Put32(diagnostics + 208, uint32_t(status_result_));
    for (unsigned i = 0; i < 3; ++i) {
        Put32(diagnostics + 212 + 4 * i, status_values_[i]);
    }
    return Error::None;
}
}  // namespace dust::ecat
