/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/imu_heater.hpp"

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "drivers/heater_pid.hpp"
#include "drivers/board_pwm.h"
#include <zephyr/sys/util.h>
#if defined(CONFIG_BOARD_DUST_HPM5E31)
#include <soc.h>
#include "hpm_common.h"
#include "hpm_soc_ip.h"
#include "hpm_ioc_regs.h"
#endif

namespace {
const pwm_dt_spec heater = PWM_DT_SPEC_GET(DT_ALIAS(onboard_imu_heater));
K_MUTEX_DEFINE(temperature_mutex);
float temperature = 0;
int64_t sample_ms = 0;
bool has_sample = false;
int64_t boot_ms = 0;
int64_t previous_ms = 0;
atomic_t heater_fault;
platform::HeaterPid controller;

void UpdateHeater(k_work*);
K_WORK_DELAYABLE_DEFINE(heater_work, UpdateHeater);

void UpdateHeater(k_work*) {
    const int64_t now = k_uptime_get();
    k_mutex_lock(&temperature_mutex, K_FOREVER);
    const float measured = temperature;
    const bool fresh = has_sample && now - sample_ms <= 200;
    k_mutex_unlock(&temperature_mutex);
    if (!fresh && now - boot_ms > 1000) {
        controller.Fault();
    }
    float duty = 0;
    if (fresh) {
        duty = controller.Update(measured, CONFIG_DUST_IMU_TARGET_C,
                                 float(now - previous_ms) / 1000.0f);
    }
    previous_ms = now;
#if !defined(CONFIG_DUST_IMU_HEATER_ENABLE)
    duty = 0;
#endif
    if (dust_board_pwm_set(&heater, uint32_t(heater.period * duty))) {
        controller.Fault();
        (void)dust_board_pwm_set(&heater, 0);
    }
    atomic_set(&heater_fault, controller.Faulted());
    k_work_reschedule(&heater_work,
                      K_MSEC(CONFIG_DUST_IMU_HEATER_PERIOD_MS));
}
}  // namespace
#endif

namespace platform {
int InitializeImuHeater() {
#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    if (!pwm_is_ready_dt(&heater)) {
        return -ENODEV;
    }
#if defined(CONFIG_BOARD_DUST_HPM5E31)
    /* PD04 defaults to the GPIO heater gate; route it to PWM0_P_4 so the
     * PWM0 channel 4 output actually reaches the heater MOSFET. */
    HPM_IOC->PAD[IOC_PAD_PD04].FUNC_CTL =
        IOC_PAD_FUNC_CTL_ALT_SELECT_SET(16);
    HPM_IOC->PAD[IOC_PAD_PD04].PAD_CTL =
        (HPM_IOC->PAD[IOC_PAD_PD04].PAD_CTL & ~0x7U) | 0x7U;
#endif
    int rc = dust_board_pwm_set(&heater, 0);
    if (rc) {
        return rc;
    }
    controller.kp = CONFIG_DUST_IMU_HEATER_KP_MILLI / 1000.0f;
    controller.ki = CONFIG_DUST_IMU_HEATER_KI_MILLI / 1000.0f;
    controller.kd = CONFIG_DUST_IMU_HEATER_KD_MILLI / 1000.0f;
    controller.max_duty = CONFIG_DUST_IMU_HEATER_MAX_PERMILLE / 1000.0f;
    boot_ms = previous_ms = k_uptime_get();
    k_work_reschedule(&heater_work,
                      K_MSEC(CONFIG_DUST_IMU_HEATER_PERIOD_MS));
#endif
    return 0;
}
void ObserveImuTemperature(float temperature_c) {
#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    k_mutex_lock(&temperature_mutex, K_FOREVER);
    temperature = temperature_c;
    sample_ms = k_uptime_get();
    has_sample = true;
    k_mutex_unlock(&temperature_mutex);
#endif
}
bool ImuHeaterFaulted() {
#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    return atomic_get(&heater_fault) != 0;
#else
    return false;
#endif
}
}  // namespace platform

/* C entry points for main.c / io_backend.c callers. */
extern "C" int dust_imu_heater_init(void) {
    return platform::InitializeImuHeater();
}
extern "C" void dust_imu_heater_observe(float temperature_c) {
    platform::ObserveImuTemperature(temperature_c);
}
extern "C" int dust_imu_heater_faulted(void) {
    return platform::ImuHeaterFaulted() ? 1 : 0;
}
