/* SPDX-License-Identifier: Apache-2.0 */

/* SSC/网关生命周期：唯一持有启动、复位与 rearm 顺序及 IRQ 门控状态。 */

#include "ecat/runtime.h"

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "applInterface.h"
#include "ecat/esc/esc_hw.h"
#include "ecat/gateway.h"
#include "ecatappl.h"

static atomic_t stack_ready;
static atomic_t reset_requested;
static int64_t next_link_poll;

/* ESC PDI 只在 SSC 栈就绪后转发给 PDI_Isr；ESC reset 延迟到主循环处理。 */
static void pdi_hook(void)
{
	if (atomic_get(&stack_ready)) {
		PDI_Isr();
	}
}

static void reset_hook(void)
{
	atomic_set(&reset_requested, 1);
}

static int stack_start(void)
{
	if (MainInit() != 0U) {
		return -1;
	}
	if (gateway_init() != 0) {
		return -2;
	}
	return 0;
}

static void stack_stop(void)
{
	atomic_clear(&stack_ready);
	esc_stop();
}

static void stack_enable(void)
{
	atomic_set(&stack_ready, 1);
	esc_start();
}

int ecat_runtime_init(void)
{
	esc_set_pdi_hook(pdi_hook);
	esc_set_reset_hook(reset_hook);
	return esc_init();
}

int ecat_runtime_start(void)
{
	return stack_start();
}

void ecat_runtime_enable(void)
{
	stack_enable();
}

ecat_step_t ecat_runtime_step(void)
{
	if (atomic_cas(&reset_requested, 1, 0)) {
		stack_stop();
		if (stack_start() != 0) {
			printk("Gateway reset failed; IRQs remain disabled\n");
			return ECAT_STEP_FATAL;
		}
		stack_enable();
		printk("SSC reset handled\n");
	}

	if (atomic_get(&stack_ready) && gateway_rearm_pending()) {
		/* 一次会话结束后 Core 进入 Protected，主站下一次启动的 0x2100
		 * 配置会被拒绝。这里在安全上下文重建网关（新的 boot_id、清空
		 * 映射/判据），使连续多次启动不再需要断电或 ESC 复位。 */
		stack_stop();
		if (gateway_rearm() != 0) {
			printk("Gateway rearm failed; IRQs remain disabled\n");
			return ECAT_STEP_FATAL;
		}
		stack_enable();
	}

	gateway_poll();
	MainLoop();
	/* MainLoop 期间可能产生新的输入映射，再 poll 一次以及时发布反馈。 */
	gateway_poll();

	const int64_t now = k_uptime_get();
	if (now >= next_link_poll) {
		esc_update_link();
		next_link_poll = now + 100;
	}
	return gateway_active() ? ECAT_STEP_ACTIVE : ECAT_STEP_IDLE;
}
