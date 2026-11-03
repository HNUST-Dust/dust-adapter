/* SPDX-License-Identifier: Apache-2.0 */

/* 会话状态机：Discover/Arm/Stop、epoch/exchange、授权与期限。
 * CAN/UART 子协议由 CanChannel/UartChannel 承担；线编解码在 codec.cpp。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/types.hpp"
#include "common/abi.hpp"
#include "domain/pdo/layout.hpp"
#include "domain/protocol/can_channel.hpp"
#include "domain/protocol/types.hpp"
#include "domain/protocol/uart_channel.hpp"

namespace ecat {

/* 单个执行上下文拥有整个核心；ISR/业务线程通过集成层队列交换数据。 */
class Session {
   public:
    explicit Session(uint32_t boot_id, Protection protection = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /* reset/rearm：换新 boot_id、清空映射/判据/过程影像与故障。 */
    void Reset(uint32_t boot_id, Protection protection);
    void InstallProtection(Protection protection);

    Error Configure(const ConfigurationTransaction& transaction,
                    const Limits& limits, const ImageLayout& rx,
                    const ImageLayout& tx);
    Error Process(Bytes image, uint64_t now_us, bool operational,
                  Actions& actions);
    void Tick(uint64_t now_us, bool operational,
              const char* file = __builtin_FILE(),
              unsigned line = __builtin_LINE());
    void Protect(Error reason, const char* file = __builtin_FILE(),
                 unsigned line = __builtin_LINE());
    /* 在 UART 后端已复制整个块并预留发送空间后调用，才推进累计 ACK。 */
    bool CommitUart(unsigned port, uint32_t sequence);
    /* 上行窗口满返回 false；调用方须累计丢弃并显式标记流中断。 */
    bool QueueUart(unsigned port, Bytes bytes, uint32_t capture_us,
                   uint64_t now_us, bool discontinuity = false);
    void CompleteStatus(uint32_t sequence, int32_t result,
                        std::array<uint32_t, 3> values);

    BusinessState state() const {
        return state_;
    }
    Error fault() const {
        return fault_;
    }
    const Layout& layout() const {
        return layout_;
    }
    uint32_t session() const {
        return session_;
    }
    uint32_t active_sources() const {
        return source_mask_;
    }
    uint32_t UartAck(unsigned port) const;
    bool UartAckValid(unsigned port) const;

    /* 显式小端编码；每次成功发布推进 publish_sequence，不分配堆内存。
     * advance_uart=false 保留上一份有效镜像的 UART 区域及窗口状态，供 CAN 额外发布。 */
    Error Publish(MutableBytes image, uint64_t now_us,
                  const Feedback& feedback, bool advance_uart = true);

   private:
    Configuration config_{};
    Limits limits_{};
    Layout layout_{};
    Protection protection_{};
    BusinessState state_ = BusinessState::Unconfigured;
    Error fault_ = Error::None;
    uint32_t boot_ = 0, session_ = 0;
    uint64_t exchange_at_ = 0;
    uint32_t exchange_session_ = 0;
    uint32_t source_mask_ = 0, exchange_ = 0, exchange_digest_ = 0;
    bool have_exchange_ = false;
    CanChannel can_channel_{};
    UartChannel uart_channel_{};
    uint32_t publish_sequence_ = 0, rejected_ = 0;
    uint32_t status_request_ = 0, status_reply_ = 0;
    int32_t status_result_ = 0;
    std::array<uint32_t, 3> status_values_{};
};

}  // namespace ecat
