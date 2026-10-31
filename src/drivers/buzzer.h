/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Thread context only. A pulse is asynchronous and bounded to 2000 ms. */
int dust_buzzer_init(void);
int dust_buzzer_beep(uint32_t duration_ms);
int dust_buzzer_stop(void);
#ifdef __cplusplus
}
#endif
