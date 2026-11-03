/* SPDX-License-Identifier: Apache-2.0 */

/* UART 子协议：Rx 交付/累计 ACK、Tx 四块窗口与重传轮转。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/types.hpp"
#include "domain/pdo/layout.hpp"
#include "domain/protocol/types.hpp"

namespace ecat {

class UartChannel {
   public:
    void Reset();
    Error Tick(uint64_t now) const;

    struct Staged {
        std::array<uint32_t, 6> ack{};
        std::array<bool, 6> have_ack{};
    };
    Error Parse(Bytes image, const RxHeader& header, const Layout& layout,
                const Configuration& config, Actions& actions,
                Staged& staged) const;
    void Commit(const Actions& actions, const Staged& staged);

    bool Queue(unsigned port, Bytes bytes, uint32_t capture_us, uint64_t now,
               bool discontinuity);
    bool CommitDelivery(unsigned port, uint32_t sequence);
    uint32_t Ack(unsigned port) const;
    bool AckValid(unsigned port) const;
    void ClearOffers();

    void Encode(MutableBytes image, const Layout& layout,
                const Configuration& config, uint64_t now, bool advance_uart,
                bool active);

   private:
    struct Block {
        uint32_t sequence = 0, capture = 0;
        uint64_t queued_at = 0, sent_at = 0;
        uint16_t length = 0;
        bool sent = false, discontinuity = false;
        std::array<uint8_t, 512> data{};
    };
    struct Window {
        std::array<Block, 4> blocks{};
        unsigned head = 0, count = 0, cursor = 0;
        uint32_t next = 1, last_ack = 0;
        bool have_ack = false;
        bool CanAck(uint32_t value) const;
        void Ack(uint32_t value);
    };

    std::array<Window, 6> windows_{};
    std::array<uint32_t, 6> uart_ack_{}, uart_offered_{};
    std::array<bool, 6> have_uart_ack_{}, have_uart_offer_{};
};

}  // namespace ecat
