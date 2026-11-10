/* SPDX-License-Identifier: Apache-2.0 */

#include "adapters/ethercat_ssc/layout_service.hpp"
#include "common/device_pdo.hpp"
#include "common/device_sensor_page.hpp"

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
    const auto match = [](const ImageLayout& image, uint16_t object,
                          uint16_t mapping) {
        size_t bytes = 60;
        unsigned count = 6;
        namespace sensor = dust::device_sensor_page;
        if (object == 0x3051 && image.bytes >= 60 &&
            image.bytes <= sensor::kSweepMaxBytes && !(image.bytes & 1U)) {
            bytes = image.bytes;
            count += (bytes - 60 + sensor::kSweepEntryBytes - 1) /
                     sensor::kSweepEntryBytes;
        }
        if (image.bytes != bytes || image.count != count || image.mapping_count != 1 ||
            image.first_pdo != mapping || image.mappings[0].index != mapping ||
            image.mappings[0].count != count)
            return false;
        size_t offset = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto& e = image.entries[i];
            const auto width = i < 6 ? 10U : std::min<size_t>(30, bytes - offset);
            if (e.index != object || e.subindex != i + 1 ||
                e.bits != width * 8 || e.offset != offset)
                return false;
            offset += width;
        }
        return true;
    };
    const auto& cfg = transaction_.candidate();
    if (mapped_rx_.bytes != 60 || mapped_tx_.bytes != 180 || cfg.cycle_us != 125 || cfg.can_tx != std::array<uint8_t, 2>{0, 0} ||
        cfg.can_rx != dust::device_pdo::kCanRxCounts ||
        !cfg.imu || cfg.signature != dust::device_sensor_page::kSignature ||
        !match(mapped_rx_, 0x2050, 0x1600) ||
        !match(mapped_tx_, 0x3051, 0x1a00)) {
        return Error::Mapping;
    }
    for (const auto& port : cfg.uart) {
        if (port.rx_bytes || port.tx_bytes) {
            return Error::Mapping;
        }
    }
    // DTG1 physical layout always reserves the compact onboard page. An explicit
    // no-sensor build may omit only its logical resource, never claim it ready.
    auto resources = cfg;
#if defined(CONFIG_ECAT_ALLOW_MISSING_ONBOARD_IMU)
    resources.imu = cfg.imu && limits.imu;
#endif
    return BuildLayout(resources, limits, prepared_);
}

void LayoutService::Freeze() {
    dictionary_.Freeze();
}

}  // namespace ecat
