/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DUST_WAIT_OTHER, DUST_WAIT_SLEEP, DUST_WAIT_PDI, DUST_WAIT_SYNC,
       DUST_WAIT_POLL1, DUST_WAIT_SSC, DUST_WAIT_POLL2, DUST_WAIT_LINK,
       DUST_WAIT_PHY, DUST_WAIT_IDLE, DUST_WAIT_PHASES };
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
void dust_wait_reset(void);
void dust_wait_phase(unsigned phase);
void dust_wait_irq_enter(void);
void dust_wait_notify(uint32_t at);
void dust_wait_wake(uint32_t begin, uint32_t end);
void dust_wait_take(uint32_t at, bool active);
void dust_wait_complete(uint32_t start, uint32_t end, bool active);
void dust_wait_worker(uint32_t start, uint32_t end);
void dust_wait_irq_interval(uint32_t start, uint32_t end);
#endif
#ifdef __cplusplus
}
#endif
