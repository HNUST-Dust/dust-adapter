/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/pwm/outputs.hpp"

#include <algorithm>
#include <cerrno>

#include <zephyr/kernel.h>

#include "drivers/pwm/board_pwm.h"
#include "drivers/pwm/buzzer.h"

namespace ecat {

ZephyrOutputs::ZephyrOutputs(const HeaterConfig& config)
    : config_(config) {
    pid_.kp = config_.kp;
    pid_.ki = config_.ki;
    pid_.kd = config_.kd;
    pid_.max_duty = config_.max_duty;
}

int ZephyrOutputs::Init() {
    if (!pwm_is_ready_dt(&heater_)) {
        return -ENODEV;
    }
    boot_us_ = previous_us_ = k_ticks_to_us_floor64(k_uptime_ticks());
    return SetHeaterDuty(0);
}

void ZephyrOutputs::ObserveTemperature(float temperature_c,
                                       uint64_t capture_us) {
    temperature_ = temperature_c;
    sample_us_ = capture_us;
    have_sample_ = true;
}

void ZephyrOutputs::Tick(uint64_t now_us) {
    if (now_us - previous_us_ < uint64_t(config_.period_ms) * 1000U) {
        return;
    }
    const bool fresh = have_sample_ && now_us - sample_us_ <= kSampleFreshUs;
    if (!fresh && now_us - boot_us_ > kBootGraceUs) {
        pid_.Fault();
    }
    float duty = 0;
    if (fresh && !pid_.Faulted()) {
        const float dt = float(now_us - previous_us_) / 1000000.0f;
        duty = pid_.Update(temperature_, config_.target_c, dt);
    }
    previous_us_ = now_us;
    if (!config_.enabled) {
        duty = 0;
    }
    if (SetHeaterDuty(duty)) {
        pid_.Fault();
        (void)SetHeaterDuty(0);
    }
    duty_ = duty;
    faulted_ = pid_.Faulted();
}

int ZephyrOutputs::SetHeaterDuty(float duty) {
    const float clamped = std::clamp(duty, 0.0f, 1.0f);
    const uint32_t pulse = uint32_t(float(heater_.period) * clamped);
    return board_pwm_set(&heater_, pulse);
}

void ZephyrOutputs::SetBuzzer(uint32_t duration_ms) {
    (void)buzzer_beep(duration_ms);
}

void ZephyrOutputs::SafeState() {
    duty_ = 0;
    (void)SetHeaterDuty(0);
}

}  // namespace ecat
