/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr SPI IMU 端口实现：包装 drivers::ImuSensor，转换时间戳/温度。 */
#pragma once

#include "drivers/spi_imu/imu_sensor.hpp"
#include "common/types.hpp"

namespace ecat {

class ImuSource {
   public:
    int Init();
    bool Poll(ImuSample& out);

   private:
    drivers::ImuSensor sensor_;
    uint32_t sequence_ = 0;
};

}  // namespace ecat
