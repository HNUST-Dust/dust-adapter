/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "usb_protocol.hpp"
namespace dust::wire {
// 会话策略独立于硬件，便于在主机上测试
// 租约和序号行为。

/**
 * @brief 独立于硬件的租约与序号状态；所有访问必须由同一所有者执行。
 */
class SessionPolicy {
   public:
    /**
     * @brief 重置策略并设置会话随机标识；调用者必须拒绝零会话标识。
     */
    void Open(uint32_t nonce) {
        *this = {};
        session_ = nonce;
    }

    /**
     * @brief 记录已接纳的序号，不续租。
     */
    void Observe(uint32_t seq) {
        last_sequence_ = seq;
        seen_ = true;
    }

    /**
     * @brief 在授权转发前设置 1000..1000000 微秒租约；状态或范围无效时返回
     * false。
     */
    bool Configure(uint32_t timeout_us) {
        if (!session_ || armed_ || timeout_us < 1000 || timeout_us > 1000000) {
            return false;
        }
        timeout_ = timeout_us;
        configured_ = true;
        return true;
    }

    /**
     * @brief 判断 seq 是否晚于上次观察到的序号。
     */
    bool Fresh(uint32_t seq) const {
        return !seen_ || NewSequence(seq, last_sequence_);
    }

    /**
     * @brief 使用新序号授权已配置的策略转发，并从 now（微秒）开始计租。
     */
    bool Arm(uint32_t seq, uint32_t now) {
        if (!configured_ || armed_ || !Fresh(seq)) {
            return false;
        }
        armed_ = true;
        last_receive_ = now;
        Observe(seq);
        return true;
    }

    /**
     * @brief 接纳新流量并续租；未授权、序号陈旧或租约过期时返回 false。
     */
    bool Accept(uint32_t seq, uint32_t now) {
        if (!armed_ || !Fresh(seq) || Expired(now)) {
            return false;
        }
        last_sequence_ = seq;
        last_receive_ = now;
        seen_ = true;
        return true;
    }

    /**
     * @brief 使用可处理回绕的无符号微秒减法检查已授权会话的租约。
     */
    bool Expired(uint32_t now) const {
        return armed_ && uint32_t(now - last_receive_) >= timeout_;
    }

    /**
     * @brief 重置会话状态并撤销转发许可。
     */
    void Close() {
        *this = {};
    }

    /**
     * @brief 返回当前会话标识；零表示未激活。
     */
    uint32_t Session() const {
        return session_;
    }

    /**
     * @brief 返回是否已授予转发许可。
     */
    bool Armed() const {
        return armed_;
    }

    /**
     * @brief 返回是否已配置租约。
     */
    bool Configured() const {
        return configured_;
    }

   private:
    uint32_t session_ = 0, timeout_ = 0, last_sequence_ = 0, last_receive_ = 0;
    bool configured_ = false, armed_ = false, seen_ = false;
};
}  // namespace dust::wire
