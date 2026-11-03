/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 子协议：Rx 命令校验/digest/来源租约，Tx 反馈编码。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/types.hpp"
#include "domain/pdo/layout.hpp"
#include "domain/protocol/types.hpp"

namespace ecat {

class CanChannel {
   public:
    struct Source {
        bool present = false;
        uint32_t sequence = 0;
        uint64_t deadline = 0;
        uint32_t digest = 0;
    };
    struct Staged {
        std::array<Source, 2> sources{};
    };

    void Reset();
    bool LeaseExpired(uint64_t now) const;

    Error Parse(Bytes image, const RxHeader& header, const Layout& layout,
                const Configuration& config, const Limits& limits,
                const Protection& protection, uint64_t now, bool arming,
                Actions& actions, Staged& staged) const;
    void Commit(const Staged& staged);

    void Encode(MutableBytes image, const Layout& layout,
                const Configuration& config, const Feedback& feedback) const;

   private:
    std::array<Source, 2> sources_{};
};

}  // namespace ecat
