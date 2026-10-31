/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void dust_pdi_reset(void);
void dust_pdi_notify(void);
bool dust_pdi_service(void (*handler)(void));
/* Read-only snapshot; does not acknowledge or consume an event. */
bool dust_pdi_pending(void);
void dust_pdi_enable(void);
void dust_pdi_disable(void);
void dust_pdi_wait(void);
void dust_pdi_wake(void);
void dust_pdi_cycle(void);
#ifdef __cplusplus
}
#endif
