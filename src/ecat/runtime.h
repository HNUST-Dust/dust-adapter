/* SPDX-License-Identifier: Apache-2.0 */

#ifndef DUST_ECAT_RUNTIME_H_
#define DUST_ECAT_RUNTIME_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 每一步主循环的结果，供应用决定空闲等待策略。 */
typedef enum {
	DUST_ECAT_STEP_IDLE = 0,
	DUST_ECAT_STEP_ACTIVE = 1,
	DUST_ECAT_STEP_FATAL = 2,
} ecat_step_t;

/* ESC/PHY/SII 启动并注册 PDI/RESET 钩子；成功返回 0。 */
int ecat_runtime_init(void);

/* SSC MainInit + 网关初始化；-1 = MainInit 失败，-2 = 网关初始化失败。 */
int ecat_runtime_start(void);

/* 置位 stack_ready 并启动 FreeRun。 */
void ecat_runtime_enable(void);

/* 处理 reset/rearm，推进网关与 SSC；返回主循环状态。 */
ecat_step_t ecat_runtime_step(void);

#ifdef __cplusplus
}
#endif

#endif /* DUST_ECAT_RUNTIME_H_ */
