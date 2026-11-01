/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
/* 使用目标机的整数字宽，LP64 测试机的 unsigned long 为 64 位。 */
#define UINT32 uint32_t
#define INT32 int32_t
#define ECAT_ESC_HW_H_
#ifdef __cplusplus
extern "C" {
#endif
void esc_read(volatile void*, uint16_t, uint16_t);
void esc_write(const volatile void*, uint16_t, uint16_t);
bool k_is_in_isr(void);
void esc_enter_critical(void);
void esc_leave_critical(void);
uint64_t k_cycle_get_64(void);
int64_t k_uptime_ticks(void);
uint64_t k_ticks_to_us_floor64(uint64_t);
int printk(const char*, ...);
#ifdef __cplusplus
}
#endif
