// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>

#include "common/paged_wire.hpp"
#include "hardware/ethercat/transport.hpp"
namespace dust_control::ethercat {
struct PagedStatistics {
    std::array<uint64_t, 5> unique_pages{};
    uint64_t skipped_publications = 0, discarded_uart_test_pages = 0;
};
class PagedTransport final : public Transport {
    std::unique_ptr<Transport> real_ = CreateIghTransport();
    std::vector<BoardImage> physical_;
    dust::paged::Receiver receiver_;
    uint32_t boot_ = 0, session_ = 0, sequence_ = 0;
    bool have_ = false, drop_one_ = false, holding_drop_ = false;
    uint32_t dropped_sequence_ = 0;
    PagedStatistics& stats_;

   public:
    explicit PagedTransport(PagedStatistics& stats, bool drop_one = false)
        : drop_one_(drop_one), stats_(stats) {}
    bool Open(uint32_t index, std::vector<BoardImage>& boards,
              std::string& error) override {
        if (boards.size() != 1) {
            error = "Paged lab supports one board";
            return false;
        }
        const auto& c = boards[0].config;
        if (boards[0].layout.tx.bytes != 410 ||
            boards[0].layout.rx.bytes != 131 ||
            boards[0].layout.cycle_time_us != 125 ||
            c.can_tx_slots != std::array<uint8_t, 2>{6, 0} ||
            c.can_rx_slots != std::array<uint8_t, 4>{3, 0, 0, 3} ||
            !c.onboard_imu) {
            error = "Paged lab requires production logical profile";
            return false;
        }
        for (unsigned i = 0; i < 6; ++i) {
            if (c.uart[i].tx_bytes || c.uart[i].rx_bytes != (i == 1   ? 128
                                                             : i == 4 ? 32
                                                                      : 0)) {
                error = "Paged lab UART capacity mismatch";
                return false;
            }
        }
        receiver_.Reset();
        stats_ = {};
        have_ = false;
        physical_ = boards;
        auto& board = physical_[0];
        board.layout.layout_signature ^= dust::paged::kSignatureXor;
        board.layout.tx = {};
        board.layout.tx.bytes = dust::paged::kBytes;
        PdoMapping mapping;
        mapping.index = 0x1a00;
        for (unsigned i = 0; i < 8; ++i) {
            mapping.entries.push_back({"page.chunk" + std::to_string(i),
                                       dust::paged::kObject, uint8_t(i + 1),
                                       192, i * 24});
        }
        board.layout.tx.mappings.push_back(std::move(mapping));
        board.input.assign(dust::paged::kBytes, 0);
        return real_->Open(index, physical_, error);
    }
    bool Receive(std::vector<BoardImage>& boards, std::string& error) override {
        if (!real_->Receive(physical_, error)) {
            return false;
        }
        auto& board = boards[0];
        const auto& raw = physical_[0];
        board.operational = raw.operational;
        board.complete = raw.complete;
        if (!board.operational || !board.complete) {
            return true;
        }
        dust::paged::View view;
        if (!dust::paged::Decode(raw.input.data(), raw.input.size(), view)) {
            error = "Invalid typed TX page";
            return false;
        }
        const auto boot = dust::paged::Get32(raw.input.data()),
                   session = dust::paged::Get32(raw.input.data() + 4),
                   sequence = dust::paged::Get32(raw.input.data() + 8);
        bool fresh = !have_ || boot != boot_ || session != session_ ||
                     sequence != sequence_;
        if (fresh) {
            if (have_ && boot == boot_ && session == session_) {
                const uint32_t delta = sequence - sequence_;
                if (delta > 1 && delta < 0x80000000) {
                    stats_.skipped_publications += delta - 1;
                }
            }
            ++stats_.unique_pages[unsigned(view.kind)];
            boot_ = boot;
            session_ = session;
            sequence_ = sequence;
            have_ = true;
        }
        if (drop_one_ && fresh && view.kind == dust::paged::Kind::Uart &&
            view.channel == 1 && (view.payload[14] & 1)) {
            drop_one_ = false;
            holding_drop_ = true;
            dropped_sequence_ = sequence;
            ++stats_.discarded_uart_test_pages;
        }
        if (holding_drop_ && sequence == dropped_sequence_) {
            std::copy_n(receiver_.data(), 410, board.input.data());
            return true;
        }
        holding_drop_ = false;
        if (!receiver_.Accept(raw.input.data(), raw.input.size())) {
            error = "Paged TX sequence/content rejected";
            return false;
        }
        std::copy_n(receiver_.data(), 410, board.input.data());
        return true;
    }
    bool Send(const std::vector<BoardImage>& boards,
              std::string& error) override {
        if (boards.size() != 1 ||
            boards[0].output.size() != physical_[0].output.size()) {
            error = "Paged RX size mismatch";
            return false;
        }
        std::copy(boards[0].output.begin(), boards[0].output.end(),
                  physical_[0].output.begin());
        return real_->Send(physical_, error);
    }
    void Close() override {
        real_->Close();
        physical_.clear();
        receiver_.Reset();
    }
};
}  // namespace dust_control::ethercat
