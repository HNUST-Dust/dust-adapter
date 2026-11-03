/* SPDX-License-Identifier: Apache-2.0 */

#include "domain/protocol/session.hpp"

#include <algorithm>
#include <cstring>

#include "domain/protocol/digest.hpp"
#include "domain/protocol/wire.hpp"
#include <pdo_schema.hpp>

namespace ecat {
namespace pdo = pdo;

Session::Session(uint32_t boot_id, Protection protection)
    : protection_(protection), boot_(boot_id) {}

Session::~Session() {
    if (state_ == BusinessState::Active) {
        Protect(Error::State);
    }
}

void Session::Reset(uint32_t boot_id, Protection protection) {
    config_ = Configuration{};
    limits_ = Limits{};
    layout_ = Layout{};
    protection_ = protection;
    state_ = BusinessState::Unconfigured;
    fault_ = Error::None;
    boot_ = boot_id;
    session_ = 0;
    exchange_at_ = 0;
    exchange_session_ = 0;
    source_mask_ = 0;
    exchange_ = 0;
    exchange_digest_ = 0;
    have_exchange_ = false;
    can_channel_.Reset();
    uart_channel_.Reset();
    publish_sequence_ = 0;
    rejected_ = 0;
    status_request_ = 0;
    status_reply_ = 0;
    status_result_ = 0;
    status_values_.fill(0);
}

void Session::InstallProtection(Protection protection) {
    protection_ = protection;
}

Error Session::Configure(const ConfigurationTransaction& transaction,
                         const Limits& limits, const ImageLayout& rx,
                         const ImageLayout& tx) {
    if (!boot_) {
        return Error::Argument;
    }
    if (state_ != BusinessState::Unconfigured) {
        return Error::State;
    }
    const Error result = transaction.Commit(limits, rx, tx, layout_);
    if (result != Error::None) {
        return result;
    }
    config_ = transaction.candidate();
    limits_ = limits;
    state_ = BusinessState::Ready;
    return Error::None;
}

void Session::Protect(Error reason, const char* file, unsigned line) {
    if (state_ == BusinessState::Protected) {
        return;
    }
    (void)file;
    (void)line;
    fault_ = reason == Error::None ? Error::Protection : reason;
    state_ = BusinessState::Protected;
    uart_channel_.ClearOffers();
    /* 回调在当前执行上下文同步调用；集成层必须使本地保护实际生效。 */
    if (protection_.stop) {
        protection_.stop(protection_.context, fault_);
    }
}

void Session::Tick(uint64_t now, bool operational, const char* file,
                   unsigned line) {
    (void)file;
    (void)line;
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
    if (can_channel_.LeaseExpired(now)) {
        Protect(Error::Lease);
        return;
    }
    if (uart_channel_.Tick(now) != Error::None) {
        Protect(Error::UartTimeout);
    }
}

Error Session::Process(Bytes image, uint64_t now, bool operational,
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

    RxHeader header;
    const Error decode = DecodeHeader(image, header);
    if (decode != Error::None || image.size != layout_.rx.bytes) {
        return reject(Error::Wire);
    }
    if (header.flags == kStop) {
        if (!Zero(image.data + 28, image.size - 28)) {
            return reject(Error::Wire);
        }
        if (state_ == BusinessState::Active &&
            (header.session != session_ || header.boot != boot_)) {
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
    if (header.flags != kDiscover && header.boot != boot_) {
        return reject(Error::Session);
    }
    if (header.flags == kDiscover && header.boot && header.boot != boot_) {
        return reject(Error::Session);
    }
    if (state_ == BusinessState::Active &&
        (header.session != session_ || header.sources != source_mask_ ||
         header.flags != kArm)) {
        return reject(Error::Session);
    }

    const uint32_t digest = Digest(image);
    if (have_exchange_ && exchange_session_ == header.session) {
        if (header.exchange == exchange_) {
            if (digest != exchange_digest_) {
                return reject(Error::Sequence);
            }
            return Error::None;  // 重复 PDO 不续看门狗，也不重新派发。
        }
        if (!Newer(header.exchange, exchange_)) {
            return reject(Error::Sequence);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((header.sources & (1U << i)) && !config_.can_tx[i]) {
            return reject(Error::Capacity);
        }
    }
    if (header.flags == kArm &&
        (!protection_.installed || !protection_.stop ||
         ((config_.can_tx[0] || config_.can_tx[1]) &&
          !protection_.validate_can))) {
        return reject(Error::Protection);
    }

    const bool arming =
        header.flags == kArm && state_ == BusinessState::Ready;

    CanChannel::Staged can_staged;
    Error error = can_channel_.Parse(image, header, layout_, config_, limits_,
                                     protection_, now, arming, actions,
                                     can_staged);
    if (error != Error::None) {
        return reject(error);
    }

    UartChannel::Staged uart_staged;
    error = uart_channel_.Parse(image, header, layout_, config_, actions,
                                uart_staged);
    if (error != Error::None) {
        return reject(error);
    }

    if (arming) {
        session_ = header.session;
        source_mask_ = header.sources;
        state_ = BusinessState::Active;
    }
    if (header.flags == kArm) {
        can_channel_.Commit(can_staged);
    }
    uart_channel_.Commit(actions, uart_staged);

    if (header.status_sequence && header.status_sequence != status_request_) {
        status_request_ = header.status_sequence;
        actions.status_sequence = header.status_sequence;
        actions.status_target = header.status_target;
        actions.status_channel = header.status_channel;
    }
    exchange_ = header.exchange;
    exchange_digest_ = digest;
    exchange_session_ = header.session;
    have_exchange_ = true;
    exchange_at_ = now;
    return Error::None;
}

bool Session::CommitUart(unsigned port, uint32_t sequence) {
    if (state_ != BusinessState::Active || port >= 6) {
        return false;
    }
    return uart_channel_.CommitDelivery(port, sequence);
}

bool Session::QueueUart(unsigned port, Bytes bytes, uint32_t capture,
                        uint64_t now, bool discontinuity) {
    Tick(now, true);
    if (state_ != BusinessState::Active || port >= 6 || !bytes.data ||
        !bytes.size || bytes.size > config_.uart[port].rx_bytes ||
        uint32_t(uint32_t(now) - capture) >= kTimeoutUs) {
        return false;
    }
    return uart_channel_.Queue(port, bytes, capture, now, discontinuity);
}

uint32_t Session::UartAck(unsigned port) const {
    return uart_channel_.Ack(port);
}

bool Session::UartAckValid(unsigned port) const {
    return uart_channel_.AckValid(port);
}

void Session::CompleteStatus(uint32_t sequence, int32_t result,
                             std::array<uint32_t, 3> values) {
    if (sequence && sequence == status_request_) {
        status_reply_ = sequence;
        status_result_ = result;
        status_values_ = values;
    }
}

}  // namespace ecat
