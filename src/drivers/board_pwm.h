/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <zephyr/drivers/pwm.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Serialize PWM0 register updates made by board peripherals. Thread context only. */
int dust_board_pwm_set(const struct pwm_dt_spec *output, uint32_t pulse_ns);
#ifdef __cplusplus
}
#endif
