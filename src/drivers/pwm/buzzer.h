/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Thread context only. A pulse is asynchronous and bounded to 2000 ms. */
int buzzer_init(void);
int buzzer_beep(uint32_t duration_ms);
int buzzer_stop(void);
#ifdef __cplusplus
}
#endif
