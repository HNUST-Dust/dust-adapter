/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/board_indicators.hpp"

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace {
const gpio_dt_spec leds[] = {GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
                             GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
                             GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios)};
const pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_ALIAS(dust_buzzer));
atomic_t board_state;

void UpdateIndicators(k_work*);
K_WORK_DELAYABLE_DEFINE(indicator_work, UpdateIndicators);

void UpdateIndicators(k_work*) {
    const int64_t now = k_uptime_get();
    const int state = atomic_get(&board_state);
    const bool fault = (state & 2) != 0;
    for (unsigned i = 0; i < 3; ++i) {
        const bool on = fault ? i == 0 : ((state & 1) ? i == 1 : i == 2);
        (void)gpio_pin_set_dt(&leds[i], on);
    }
    // 故障时短鸣，启动和空闲不鸣叫。
    const bool beep = fault && (now % 1000 < 100);
    (void)pwm_set_dt(&buzzer, buzzer.period, beep ? buzzer.period / 2 : 0);
    k_work_reschedule(&indicator_work, K_MSEC(20));
}
}  // namespace
#endif

namespace platform {
int InitializeBoardIndicators() {
#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    for (const auto& led : leds) {
        if (!gpio_is_ready_dt(&led)) {
            return -ENODEV;
        }
        const int rc = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
        if (rc) {
            return rc;
        }
    }
    if (!pwm_is_ready_dt(&buzzer)) {
        return -ENODEV;
    }
    int rc = pwm_set_dt(&buzzer, buzzer.period, 0);
    if (rc) {
        return rc;
    }
    k_work_reschedule(&indicator_work, K_MSEC(20));
#endif
    return 0;
}
void SetBoardState(bool armed, bool fault) {
#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    atomic_set(&board_state, (armed ? 1 : 0) | (fault ? 2 : 0));
#endif
}
}  // namespace platform
