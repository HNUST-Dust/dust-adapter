/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/spi_imu/imu_source.hpp"

#include <zephyr/kernel.h>

namespace ecat {

int ImuSource::Init() {
    if (ready_) {
        sensor_.SetBurstReady([](void* context, const drivers::ImuSensor::Burst& burst) {
            auto& source = *static_cast<ImuSource*>(context);
            ImuSample sample;
            source.DecodeBurst(burst, sample);
            source.ready_(source.ready_context_, sample);
        }, this);
    }
    return sensor_.Init();
}

bool ImuSource::Poll(ImuSample& out) {
    drivers::ImuSensor::Burst burst;
    const bool taken = sensor_.TryTakeCompleted(burst);
    (void)sensor_.TryStartAsync();
    if (!taken) {
        return false;
    }
    DecodeBurst(burst, out);
    return true;
}

void ImuSource::DecodeBurst(const drivers::ImuSensor::Burst& burst, ImuSample& out) {
    out.sequence = ++sequence_;
    /* DRDY 保存的是 32 位硬件周期计数，不能直接当作 uptime 时间戳。
     * 以无符号差计算短时样本年龄，再锚定到 PDO 使用的 uptime 时域。 */
    const uint32_t age_us =
        k_cyc_to_us_floor32(k_cycle_get_32() - burst.data_ready_cycle);
    const uint64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());
    out.capture_us = uint32_t(now_us >= age_us ? now_us - age_us : 0);
    out.temperature_c = burst.temperature_c;
    for (size_t i = 0; i < out.raw.size(); ++i) {
        out.raw[i] = burst.rx[i];
    }
}

}  // namespace ecat
