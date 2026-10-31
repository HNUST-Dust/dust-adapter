/* SPDX-License-Identifier: Apache-2.0 */
#include "board_pwm.h"
#include <zephyr/kernel.h>
K_MUTEX_DEFINE(board_pwm_lock);
int dust_board_pwm_set(const struct pwm_dt_spec *output, uint32_t pulse_ns)
{
    k_mutex_lock(&board_pwm_lock, K_FOREVER);
    int rc = pwm_set_dt(output, output->period, pulse_ns);
    k_mutex_unlock(&board_pwm_lock);
    return rc;
}
