/* SPDX-License-Identifier: Apache-2.0 */

#include "domain/protocol/uart_channel.hpp"

#include <algorithm>
#include <cstring>

#include "common/abi.hpp"
#include "domain/protocol/wire.hpp"
#include <pdo_schema.hpp>

namespace ecat {
namespace pdo = pdo;

void UartChannel::Reset() {
    windows_.fill(Window{});
    uart_ack_.fill(0);
    uart_offered_.fill(0);
    have_uart_ack_.fill(false);
    have_uart_offer_.fill(false);
}

bool UartChannel::Window::CanAck(uint32_t value) const {
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

void UartChannel::Window::Ack(uint32_t value) {
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

Error UartChannel::Tick(uint64_t now) const {
    for (const auto& window : windows_) {
        for (unsigned i = 0; i < window.count; ++i) {
            const auto& b = window.blocks[(window.head + i) % 4];
            if (now < b.queued_at || now - b.queued_at >= kTimeoutUs ||
                uint32_t(uint32_t(now) - b.capture) >= kTimeoutUs ||
                (b.sent && now - b.sent_at >= kTimeoutUs)) {
                return Error::UartTimeout;
            }
        }
    }
    return Error::None;
}

Error UartChannel::Parse(Bytes image, const RxHeader& header,
                         const Layout& layout, const Configuration& config,
                         Actions& actions, Staged& staged) const {
    staged = Staged{};
    for (unsigned i = 0; i < 6; ++i) {
        if (layout.uart_rx[i] == kAbsent) {
            continue;
        }
        const auto* p = image.data + layout.uart_rx[i];
        const unsigned length = U16(p + pdo::rx::uart::length),
                       flags = p[pdo::rx::uart::flags];
        if ((flags & ~7U) || length > config.uart[i].tx_bytes ||
            (!(flags & kDataValid) && length)) {
            return Error::Wire;
        }
        if ((flags & kAckValid) &&
            !windows_[i].CanAck(U32(p + pdo::rx::uart::peer_ack_sequence))) {
            return Error::Sequence;
        }
        if ((flags & kAckValid) && layout.uart_rx[i] != kAbsent) {
            staged.ack[i] = U32(p + pdo::rx::uart::peer_ack_sequence);
            staged.have_ack[i] = true;
        }
        if (!(flags & kDataValid)) {
            continue;
        }
        if (!length || header.flags != kArm) {
            return Error::Wire;
        }
        const uint32_t sequence = U32(p + pdo::rx::uart::block_sequence);
        const uint32_t expected = have_uart_ack_[i] ? uart_ack_[i] + 1U : 1U;
        if (sequence == expected) {
            actions.uart[i] = {
                true, sequence, bool(flags & kDiscontinuity),
                {p + pdo::rx::uart::header_size, length}};
        }
    }
    return Error::None;
}

void UartChannel::Commit(const Actions& actions, const Staged& staged) {
    for (unsigned i = 0; i < 6; ++i) {
        have_uart_offer_[i] = actions.uart[i].valid;
        uart_offered_[i] = actions.uart[i].sequence;
        if (staged.have_ack[i]) {
            windows_[i].Ack(staged.ack[i]);
        }
    }
}

bool UartChannel::Queue(unsigned port, Bytes bytes, uint32_t capture_us,
                        uint64_t now, bool discontinuity) {
    auto& window = windows_[port];
    if (window.count == 4) {
        return false;
    }
    auto& b = window.blocks[(window.head + window.count) % 4];
    b = Block{};
    b.sequence = window.next++;
    b.length = uint16_t(bytes.size);
    b.capture = capture_us;
    b.queued_at = now;
    b.discontinuity = discontinuity;
    std::copy_n(bytes.data, bytes.size, b.data.begin());
    window.count++;
    return true;
}

bool UartChannel::CommitDelivery(unsigned port, uint32_t sequence) {
    if (port >= 6 || !have_uart_offer_[port] ||
        uart_offered_[port] != sequence) {
        return false;
    }
    uart_ack_[port] = sequence;
    have_uart_ack_[port] = true;
    have_uart_offer_[port] = false;
    return true;
}

uint32_t UartChannel::Ack(unsigned port) const {
    return port < 6 ? uart_ack_[port] : 0;
}

bool UartChannel::AckValid(unsigned port) const {
    return port < 6 && have_uart_ack_[port];
}

void UartChannel::ClearOffers() {
    have_uart_offer_.fill(false);
}

void UartChannel::Encode(MutableBytes image, const Layout& layout,
                         const Configuration& config, uint64_t now,
                         bool advance_uart, bool active) {
    if (!advance_uart) {
        return;
    }
    auto* p = image.data;
    for (unsigned port = 0; port < 6; ++port) {
        if (layout.uart_tx[port] == kAbsent) {
            continue;
        }
        auto* out = p + layout.uart_tx[port];
        uint8_t flags = 0;
        if (have_uart_ack_[port]) {
            flags |= kAckValid;
            Put32(out + pdo::tx::uart::peer_ack_sequence, uart_ack_[port]);
        }
        auto& w = windows_[port];
        if (active && w.count) {
            auto& b = w.blocks[(w.head + w.cursor) % 4];
            if (!b.sent) {
                b.sent = true;
                b.sent_at = now;
            }
            Put32(out + pdo::tx::uart::block_sequence, b.sequence);
            Put32(out + pdo::tx::uart::capture_time_us, b.capture);
            Put16(out + pdo::tx::uart::length, b.length);
            flags |= kDataValid | (b.discontinuity ? kDiscontinuity : 0);
            std::copy_n(b.data.begin(), b.length,
                        out + pdo::tx::uart::header_size);
            w.cursor = (w.cursor + 1) % w.count;
        }
        out[pdo::tx::uart::flags] = flags;
    }
}

}  // namespace ecat
