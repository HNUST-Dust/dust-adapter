/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ethercat/protocol.hpp"
namespace dust::ecat {
// 周期线程独占；ISR 接收队列由平台提供，不得调用本容器。
template <typename T, size_t Depth>
class IoQueue {
   public:
    bool Push(const T& value) {
        if (count_ == Depth) {
            return false;
        }
        values_[(head_ + count_++) % Depth] = value;
        return true;
    }
    T* Front() {
        return count_ ? &values_[head_] : nullptr;
    }
    const T* At(size_t i) const {
        return i < count_ ? &values_[(head_ + i) % Depth] : nullptr;
    }
    void Pop() {
        if (count_) {
            head_ = (head_ + 1) % Depth;
            --count_;
        }
    }
    size_t size() const {
        return count_;
    }
    size_t free() const {
        return Depth - count_;
    }
    void Clear() {
        head_ = count_ = 0;
    }

   private:
    std::array<T, Depth> values_{};
    size_t head_ = 0, count_ = 0;
};
struct PendingCan {
    CanFrame frame;
    uint64_t deadline_us = 0;
};
inline bool CanExpired(const PendingCan& value, uint64_t now) {
    return value.deadline_us && now >= value.deadline_us;
}
// counter 必须先持久化并读回，才可把 boot 暴露给主站；零是保留值。
inline bool NextBootIdentity(uint64_t identity, uint64_t previous,
                             uint64_t& counter, uint64_t& boot) {
    if (!identity || previous == UINT64_MAX) {
        return false;
    }
    counter = previous + 1;
    boot = identity ^ counter;
    if (!boot) {
        if (counter == UINT64_MAX) {
            return false;
        }
        boot = identity ^ ++counter;
    }
    return true;
}
}  // namespace dust::ecat
