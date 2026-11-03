/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/spi_imu/imu_source.hpp"

#include <zephyr/kernel.h>

namespace ecat {

int ImuSource::Init() {
    return sensor_.Init();
}

bool ImuSource::Poll(ImuSample& out) {
    drivers::ImuSensor::Burst burst;
    const bool taken = sensor_.TryTakeCompleted(burst);
    (void)sensor_.TryStartAsync();
    if (!taken) {
        return false;
    }
    out.sequence = ++sequence_;
    out.capture_us = k_cyc_to_us_floor32(burst.data_ready_cycle);
    out.temperature_c = burst.temperature_c;
    for (size_t i = 0; i < out.raw.size(); ++i) {
        out.raw[i] = burst.rx[i];
    }
    return true;
}

}  // namespace ecat
