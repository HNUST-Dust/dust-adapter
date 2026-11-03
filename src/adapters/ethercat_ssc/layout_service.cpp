/* SPDX-License-Identifier: Apache-2.0 */

#include "adapters/ethercat_ssc/layout_service.hpp"

namespace ecat {

void LayoutService::Reset() {
    dictionary_.Clear();
    transaction_.Clear();
    prepared_ = Layout{};
    mapped_rx_ = ImageLayout{};
    mapped_tx_ = ImageLayout{};
}

Error LayoutService::GenerateMapping(const Limits& limits) {
    const Error snapshot = dictionary_.Snapshot(mapped_rx_, mapped_tx_);
    if (snapshot != Error::None) {
        return snapshot;
    }
    return transaction_.Commit(limits, mapped_rx_, mapped_tx_, prepared_);
}

void LayoutService::Freeze() {
    dictionary_.Freeze();
}

}  // namespace ecat
