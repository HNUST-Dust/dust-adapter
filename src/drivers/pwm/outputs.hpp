/* SPDX-License-Identifier: Apache-2.0 */

/* 板级输出：IMU 加热 PID 与蜂鸣器。直接调用 PWM/驱动 API，不再经过端口封装。
 * 加热器故障属于本地维护事项，只关加热并记录，不升级为 EtherCAT 会话故障。 */
#pragma once

#include <cstdint>

#include <zephyr/drivers/pwm.h>

#include "drivers/pwm/heater_pid.hpp"
#include "common/types.hpp"

namespace ecat {

class ZephyrOutputs {
   public:
    explicit ZephyrOutputs(const HeaterConfig& config);

    int Init();
    /* 反馈聚合每个周期调用；记录最新温度供 PID 使用。 */
    void ObserveTemperature(float temperature_c, uint64_t capture_us);
    /* 慢任务周期调用：按 period_ms 限流执行 PID 与安全关断。 */
    void Tick(uint64_t now_us);
    void SetBuzzer(uint32_t duration_ms);
    void SafeState();

    bool Faulted() const {
        return faulted_;
    }
    float Duty() const {
        return duty_;
    }

   private:
    static constexpr uint64_t kSampleFreshUs = 200000;
    static constexpr uint64_t kBootGraceUs = 1000000;

    int SetHeaterDuty(float duty);

    const pwm_dt_spec heater_ = PWM_DT_SPEC_GET(DT_ALIAS(onboard_imu_heater));
    HeaterConfig config_;
    platform::HeaterPid pid_;
    bool have_sample_ = false;
    bool faulted_ = false;
    float temperature_ = 0;
    uint64_t sample_us_ = 0;
    uint64_t boot_us_ = 0;
    uint64_t previous_us_ = 0;
    float duty_ = 0;
};

}  // namespace ecat
