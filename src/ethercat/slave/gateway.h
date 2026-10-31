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
#ifdef __cplusplus
}
#endif
