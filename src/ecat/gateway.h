/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int gateway_init(void);
// 会话结束后的受控重建：调用方先屏蔽 ESC IRQ 并清除 stack_ready。
int gateway_rearm(void);
// 一次已进入 SAFEOP/OP 的会话结束后置位，主循环据此调用 rearm。
bool gateway_rearm_pending(void);
void gateway_poll(void);
void gateway_sync_fault(void);
bool gateway_active(void);
#ifdef __cplusplus
}
#endif
