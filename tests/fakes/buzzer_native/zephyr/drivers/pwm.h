#pragma once
#include <stdbool.h>
#include <stdint.h>
struct pwm_dt_spec { uint32_t period; };
extern bool fake_ready;
extern int pwm_result;
extern uint32_t pulse;
#define DT_ALIAS(x) 0
#define PWM_DT_SPEC_GET(x) { .period = 500000 }
static inline bool pwm_is_ready_dt(const struct pwm_dt_spec *s) { (void)s; return fake_ready; }
static inline int pwm_set_dt(const struct pwm_dt_spec *s, uint32_t period, uint32_t value) { (void)s; (void)period; pulse=value; return pwm_result; }
