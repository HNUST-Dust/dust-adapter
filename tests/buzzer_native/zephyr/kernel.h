#pragma once
#include <stdbool.h>
#include <stdint.h>
struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
struct k_mutex { int unused; };
extern int64_t fake_time;
extern int schedule_result;
extern int64_t scheduled_ms;
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_WORK_DELAYABLE_DEFINE(name, fn) struct k_work_delayable name
#define K_FOREVER 0
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
static inline int k_mutex_lock(struct k_mutex *m, int t) { (void)m; (void)t; return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { (void)m; return 0; }
static inline int64_t k_uptime_get(void) { return fake_time; }
static inline int k_work_reschedule(struct k_work_delayable *w, int64_t d) { (void)w; scheduled_ms=d; return schedule_result; }
