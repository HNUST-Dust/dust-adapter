/* SPDX-License-Identifier: Apache-2.0 */

/* 无堆有界队列；单执行上下文容器，不跨线程使用。 */
#pragma once

#include <array>
#include <cstddef>

namespace ecat {

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

}  // namespace ecat
