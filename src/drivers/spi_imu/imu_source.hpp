/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr SPI IMU 端口实现：包装 drivers::ImuSensor，转换时间戳/温度。 */
#pragma once

#include "drivers/spi_imu/imu_sensor.hpp"
#include "common/types.hpp"

namespace ecat {

class ImuSource {
   public:
    using SampleReady = void (*)(void*, const ImuSample&);
    void SetSampleReady(SampleReady ready, void* context) {
        ready_ = ready;
        ready_context_ = context;
    }
    int Init();
    bool Poll(ImuSample& out);
    void WaitForEvent() { sensor_.WaitForEvent(); }

   private:
    void DecodeBurst(const drivers::ImuSensor::Burst&, ImuSample&);
    SampleReady ready_ = nullptr;
    void* ready_context_ = nullptr;
    drivers::ImuSensor sensor_;
    uint32_t sequence_ = 0;
};

}  // namespace ecat
