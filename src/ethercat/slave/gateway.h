/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int dust_gateway_init(void);
void dust_gateway_poll(void);
void dust_gateway_sync_fault(void);
bool dust_gateway_active(void);
// SSC PDO input write adapter; all other accesses use the raw hardware path.
void dust_gateway_esc_write_isr(const volatile void* data, uint16_t address,
                                uint16_t length);
#if defined(CONFIG_DUST_ECAT_INPUT_WRITE_COALESCE) && defined(CONFIG_DUST_ECAT_SCHED_STATS)
// Active-state calls only; writes counts raw synchronous write attempts.
struct DustInputWriteStats {
    uint32_t version, requests, writes, skipped, failures;
};
extern struct DustInputWriteStats dust_ecat_input_write_stats;
#endif
#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH) && defined(CONFIG_DUST_ECAT_SCHED_STATS)
struct DustInputRefreshStats {
    uint32_t version, checks, refreshes, failures;
};
extern struct DustInputRefreshStats dust_ecat_input_refresh_stats;
#endif
#ifdef __cplusplus
}
#endif
