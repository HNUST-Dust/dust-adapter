#pragma once
#include <stdint.h>
extern uint64_t trace_test_clock;
static inline uint64_t k_cycle_get_64(void) { return ++trace_test_clock; }
