/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <algorithm>
#include <cmath>

namespace platform {
/** @brief 单向加热 PID；增益单位分别为占空比/℃、占空比/(℃·秒)、占空比·秒/℃。 */
class HeaterPid {
   public:
    float Update(float temperature, float target, float dt) {
        if (!std::isfinite(temperature) || temperature < -40 ||
            temperature >= 80 || !std::isfinite(dt) || dt <= 0 || dt > 0.5f) {
            fault_ = true;
        }
        if (fault_) {
            return 0;
        }
        const float error = target - temperature;
        const float derivative =
            initialized_ ? -(temperature - previous_) / dt : 0;
        previous_ = temperature;
        initialized_ = true;
        // 仅在未饱和或积分有助于退出饱和时积分，避免长时间冷启动积分累积。
        const float candidate = integral_ + ki * error * dt;
        const float output = kp * error + candidate + kd * derivative;
        if ((output >= 0 && output <= max_duty) ||
            (output > max_duty && error < 0) || (output < 0 && error > 0)) {
            integral_ = candidate;
        }
        return std::clamp(output, 0.0f, max_duty);
    }
    void Fault() {
        fault_ = true;
    }
    bool Faulted() const {
        return fault_;
    }
    // 沿用旧 AHRS 的百分比增益：kp=10、ki=0.5，内部换算为 0..1。
    float kp = 0.1f;
    float ki = 0.005f;
    float kd = 0.0f;
    float max_duty = 1.0f;

   private:
    float integral_ = 0;
    float previous_ = 0;
    bool initialized_ = false;
    bool fault_ = false;
};
}  // namespace platform
