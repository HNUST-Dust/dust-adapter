/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 适配器私有的 PDO 配置暂存：持有 0x2100 事务、CoE 映射暂存器与已校验布局。
 * 仅由 SscStack 使用，不再作为 domain 模块。 */
#pragma once

#include <cstdint>

#include "domain/pdo/layout.hpp"
#include "domain/pdo/pdo_dictionary.hpp"

namespace ecat {

class LayoutService {
   public:
    void Reset();
    /* Snapshot + Commit；成功后才更新 prepared()。 */
    Error GenerateMapping(const Limits& limits);
    void Freeze();

    const Layout& prepared() const {
        return prepared_;
    }
    const ImageLayout& mapped_rx() const {
        return prepared_.rx;
    }
    const ImageLayout& mapped_tx() const {
        return mapped_tx_;
    }
    const ImageLayout& physical_rx() const { return mapped_rx_; }
    const ImageLayout& physical_tx() const { return mapped_tx_; }
    const ConfigurationTransaction& session_transaction() const {
        return transaction_;
    }
    const ConfigurationTransaction& transaction() const {
        return transaction_;
    }
    ConfigurationTransaction& transaction() {
        return transaction_;
    }
    PdoDictionary& dictionary() {
        return dictionary_;
    }

   private:
    PdoDictionary dictionary_{};
    ConfigurationTransaction transaction_{};
    Layout prepared_{};
    ImageLayout mapped_rx_{};
    ImageLayout mapped_tx_{};
};

}  // namespace ecat
