/* SPDX-License-Identifier: Apache-2.0 */

#include "domain/protocol/can_channel.hpp"

#include <algorithm>
#include <cstring>

#include "common/abi.hpp"
#include "domain/protocol/digest.hpp"
#include "domain/protocol/wire.hpp"
#include <pdo_schema.hpp>

namespace ecat {
namespace pdo = pdo;

void CanChannel::Reset() {
    sources_.fill(Source{});
}

bool CanChannel::LeaseExpired(uint64_t now) const {
    for (const auto& source : sources_) {
        if (source.present && now >= source.deadline) {
            return true;
        }
    }
    return false;
}

void CanChannel::Commit(const Staged& staged) {
    sources_ = staged.sources;
}

Error CanChannel::Parse(Bytes image, const RxHeader& header,
                        const Layout& layout, const Configuration& config,
                        const Limits& limits, const Protection& protection,
                        uint64_t now, bool arming, Actions& actions,
                        Staged& staged) const {
    staged.sources = sources_;
    if (arming) {
        for (auto& source : staged.sources) {
            source = Source{};
            source.deadline = now + kTimeoutUs;
        }
    }

    for (unsigned source = 0; source < 2; ++source) {
        if (layout.source[source] == kAbsent) {
            continue;
        }
        const auto* p = image.data + layout.source[source];
        const unsigned flags = p[pdo::rx::can::flags];
        const unsigned count = p[pdo::rx::can::frame_count];
        if (flags > 1 || count > config.can_tx[source]) {
            return Error::Wire;
        }
        if (!flags) {
            if (!Zero(p, pdo::rx::can::header_size +
                            pdo::rx::can::frame_stride *
                                config.can_tx[source])) {
                return Error::Wire;
            }
            continue;
        }
        if (!(header.sources & (1U << source)) ||
            p[pdo::rx::can::source_id] != source ||
            !U32(p + pdo::rx::can::remaining_valid_us) ||
            U32(p + pdo::rx::can::remaining_valid_us) > kTimeoutUs) {
            return Error::Lease;
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
            if (frame.bus >= 4 || !(limits.can_mask & (1U << frame.bus)) ||
                frame.length > 8 || frame.id > 0x7ff) {
                return Error::Wire;
            }
            std::copy_n(f + pdo::rx::can::frame_data, 8, frame.data.begin());
            if (!protection.validate_can ||
                !protection.validate_can(protection.context, source, frame)) {
                return Error::Protection;
            }
        }
        if (!Zero(p + pdo::rx::can::header_size +
                      count * pdo::rx::can::frame_stride,
                  (config.can_tx[source] - count) *
                      pdo::rx::can::frame_stride)) {
            return Error::Wire;
        }
        const uint32_t batch_digest = Digest(
            {p + pdo::rx::can::flags,
             size_t(pdo::rx::can::header_size - pdo::rx::can::flags +
                    pdo::rx::can::frame_stride * config.can_tx[source])});
        auto& previous = staged.sources[source];
        if (previous.present && batch.sequence == previous.sequence) {
            if (previous.digest != batch_digest) {
                return Error::Sequence;
            }
        } else {
            if (previous.present && !Newer(batch.sequence, previous.sequence)) {
                return Error::Sequence;
            }
            previous = {true, batch.sequence,
                        now + U32(p + pdo::rx::can::remaining_valid_us),
                        batch_digest};
            batch.valid = header.flags == kArm;
            batch.deadline_us = previous.deadline;
        }
    }
    return Error::None;
}

void CanChannel::Encode(MutableBytes image, const Layout& layout,
                        const Configuration& config,
                        const Feedback& feedback) const {
    auto* p = image.data;
    for (unsigned bus = 0; bus < 4; ++bus) {
        if (layout.feedback[bus] == kAbsent) {
            continue;
        }
        auto* out = p + layout.feedback[bus];
        const auto& batch = feedback.can[bus];
        Put32(out + pdo::tx::can::batch_sequence, batch.sequence);
        Put32(out + pdo::tx::can::cumulative_drops, batch.drops);
        out[pdo::tx::can::frame_count] = uint8_t(batch.count);
        for (unsigned n = 0; n < batch.count; ++n) {
            auto* f = out + pdo::tx::can::header_size +
                      pdo::tx::can::frame_stride * n;
            const auto& event = batch.frames[n];
            Put16(f + pdo::tx::can::frame_id, uint16_t(event.id));
            Put32(f + pdo::tx::can::frame_bus_sequence, event.sequence);
            Put32(f + pdo::tx::can::frame_capture_time_us, event.capture_us);
            f[pdo::tx::can::frame_length] = event.length;
            std::copy(event.data.begin(), event.data.end(),
                      f + pdo::tx::can::frame_data);
        }
    }
}

}  // namespace ecat
