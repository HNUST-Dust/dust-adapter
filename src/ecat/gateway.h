/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int dust_gateway_init(void);
// 会话结束后的受控重建：调用方先屏蔽 ESC IRQ 并清除 stack_ready。
int dust_gateway_rearm(void);
// 一次已进入 SAFEOP/OP 的会话结束后置位，主循环据此调用 rearm。
bool dust_gateway_rearm_pending(void);
void dust_gateway_poll(void);
void dust_gateway_sync_fault(void);
bool dust_gateway_active(void);
// SSC PDO input write adapter; all other accesses use the raw hardware path.
void dust_gateway_esc_write_isr(const volatile void* data, uint16_t address,
                                uint16_t length);
#ifdef __cplusplus
}
#endif
