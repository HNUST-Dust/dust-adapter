/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
void dust_sync0_reset(void);
void dust_sync0_notify(void);
void dust_sync0_period(uint32_t cycles);
void dust_sync0_begin(void);
void dust_sync0_set_active(bool active);
unsigned dust_sync0_phase(unsigned phase);
void dust_sync0_worker_elapsed(uint32_t elapsed);
bool dust_sync0_service(void (*handler)(void));
/* Read-only snapshot; does not acknowledge or consume an event. */
bool dust_sync0_pending(void);
bool dust_sync0_error(void);
#endif
#ifdef __cplusplus
}
#endif


// Non-overlapping wall-clock phases within the deferred Sync0 owner only.
enum { DUST_SYNC_SSC, DUST_SYNC_TICK, DUST_SYNC_CAN, DUST_SYNC_PROCESS,
       DUST_SYNC_IO, DUST_SYNC_PUBLISH, DUST_SYNC_MAPPING, DUST_SYNC_ESC,
       DUST_SYNC_IO_CHECKS, DUST_SYNC_IO_CAN_TX, DUST_SYNC_IO_UART_TX,
       DUST_SYNC_CORE_PROCESS, DUST_SYNC_DISPATCH, DUST_SYNC_CAPTURE,
       DUST_SYNC_ENCODE, DUST_SYNC_TX_PREP, DUST_SYNC_DRV_VALIDATE,
       DUST_SYNC_DRV_ADMIT, DUST_SYNC_DRV_STATE, DUST_SYNC_DRV_RAM,
       DUST_SYNC_DRV_REQUEST, DUST_SYNC_DRV_RELEASE };
#ifdef __cplusplus
class DustSyncPhaseScope {
    unsigned previous_;
public:
    explicit DustSyncPhaseScope(unsigned phase) {
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
        previous_ = dust_sync0_phase(phase);
#else
        (void)phase; previous_ = 0;
#endif
    }
    ~DustSyncPhaseScope() {
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
        (void)dust_sync0_phase(previous_);
#endif
    }
};
#endif
