/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int dust_gateway_init(void);
void dust_gateway_poll(void);
bool dust_gateway_active(void);
#ifdef __cplusplus
}
#endif
