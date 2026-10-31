/* SPDX-License-Identifier: Apache-2.0 */
#include "buzzer.h"
#include "board_pwm.h"
#include <errno.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_ALIAS(pwm_buzzer));
K_MUTEX_DEFINE(buzzer_lock);
static int64_t deadline;
static bool ready;
static void buzzer_timeout(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(buzzer_work, buzzer_timeout);

static void buzzer_timeout(struct k_work *work)
{
    ARG_UNUSED(work);
    k_mutex_lock(&buzzer_lock, K_FOREVER);
    const int64_t remaining = deadline - k_uptime_get();
    if (ready && remaining > 0) {
        (void)k_work_reschedule(&buzzer_work, K_MSEC(remaining));
    } else if (ready) {
        (void)dust_board_pwm_set(&buzzer, 0);
    }
    k_mutex_unlock(&buzzer_lock);
}
int dust_buzzer_init(void)
{
    if (!pwm_is_ready_dt(&buzzer)) return -ENODEV;
    k_mutex_lock(&buzzer_lock, K_FOREVER);
    int rc = dust_board_pwm_set(&buzzer, 0);
    ready = rc == 0;
    k_mutex_unlock(&buzzer_lock);
    return rc;
}
int dust_buzzer_beep(uint32_t duration_ms)
{
    if (!duration_ms || duration_ms > 2000U) return -EINVAL;
    k_mutex_lock(&buzzer_lock, K_FOREVER);
    if (!ready) { k_mutex_unlock(&buzzer_lock); return -ENODEV; }
    int rc = dust_board_pwm_set(&buzzer, buzzer.period / 2U);
    if (!rc) {
        deadline = k_uptime_get() + duration_ms;
        rc = k_work_reschedule(&buzzer_work, K_MSEC(duration_ms));
        if (rc >= 0) rc = 0;
        else (void)dust_board_pwm_set(&buzzer, 0);
    }
    k_mutex_unlock(&buzzer_lock);
    return rc;
}
int dust_buzzer_stop(void)
{
    k_mutex_lock(&buzzer_lock, K_FOREVER);
    if (!ready) { k_mutex_unlock(&buzzer_lock); return -ENODEV; }
    deadline = 0;
    int rc = dust_board_pwm_set(&buzzer, 0);
    k_mutex_unlock(&buzzer_lock);
    return rc;
}
