/* SPDX-License-Identifier: Apache-2.0 */

/* 反馈聚合：CAN 快照 + UART 窗口 + IMU 最新样本 -> Feedback。
 * IMU 采样独立于主站是否配置 imu（热控需要），是否发布由配置决定。 */
#pragma once

#include <cstdint>

#include "domain/can/can_service.hpp"
#include "domain/protocol/session.hpp"
#include "domain/uart/uart_service.hpp"
#include "common/types.hpp"
#include "drivers/spi_imu/imu_source.hpp"

namespace ecat {

class FeedbackAggregator {
   public:
    FeedbackAggregator(CanService& can, UartService& uart, ImuSource& imu);

    int Initialize();
    bool CanCapacityValid(const Configuration& value) const;
    void Configure(const Configuration& value);

    bool DrainCan(uint64_t now);
    /* 每周期采样 IMU（热控需要），与主站是否发布无关。 */
    void PollSensors();
    /* 只采样 CAN 反馈（额外发布时保留 UART 影像与窗口）。 */
    void CaptureCan(Feedback& feedback);
    void Capture(Session& session, Feedback& feedback, uint64_t now);

    bool LatestTemperature(float& temperature_c, uint64_t& capture_us) const;

   private:
    CanService& can_;
    UartService& uart_;
    ImuSource& imu_;
    ImuSample latest_{};
    bool have_sample_ = false;
    bool imu_enabled_ = false;
    uint32_t imu_sequence_ = 0;
};

}  // namespace ecat
