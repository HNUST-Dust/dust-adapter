/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "applInterface.h"
#include "drivers/buzzer.h"
#include "ecat/esc_hw.h"
#include "ecat/gateway.h"
#include "ecatappl.h"

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
extern int dust_imu_heater_init(void);
#endif

static atomic_t stack_ready;
static atomic_t reset_requested;

/* ESC PDI 只在 SSC 栈就绪后转发给 PDI_Isr；ESC reset 由主循环延迟处理。 */
static void pdi_hook(void) {
    if (atomic_get(&stack_ready)) {
        PDI_Isr();
    }
}

static void reset_hook(void) {
    atomic_set(&reset_requested, 1);
}

/* SSC MainInit + 网关初始化；-1 = MainInit 失败，-2 = 网关初始化失败。 */
static int stack_start(void) {
    if (MainInit() != 0U) {
        return -1;
    }
    if (dust_gateway_init() != 0) {
        return -2;
    }
    return 0;
}

static void stack_stop(void) {
    atomic_clear(&stack_ready);
    dust_esc_stop();
}

static void stack_enable(void) {
    atomic_set(&stack_ready, 1);
    dust_esc_start();
}

int main(void) {
    dust_esc_set_pdi_hook(pdi_hook);
    dust_esc_set_reset_hook(reset_hook);
    if (dust_esc_init() != 0) {
        return 0;
    }

#if defined(CONFIG_DUST_BUZZER)
    const int buzzer_rc = dust_buzzer_init();
    if (buzzer_rc) {
        printk("Buzzer init failed: %d\n", buzzer_rc);
    }
#endif

    const int start_rc = stack_start();
    if (start_rc == -1) {
        printk("SSC MainInit failed\n");
        return 0;
    }
    if (start_rc != 0) {
        printk("DTC1 gateway initialization failed; IRQs remain disabled\n");
        return 0;
    }

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
    /* Start the PID only after the IMU is sampling, so a slow first sample
     * cannot latch the heater fault. */
    const int heater_rc = dust_imu_heater_init();
    if (heater_rc) {
        printk("IMU heater init failed: %d\n", heater_rc);
    }
#endif

    stack_enable();
    printk("DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
           "installed\n");
    int64_t next_link_poll = 0;

    for (;;) {
        if (atomic_cas(&reset_requested, 1, 0)) {
            stack_stop();
            if (stack_start() != 0) {
                printk("Gateway reset failed; IRQs remain disabled\n");
                return 0;
            }
            stack_enable();
            printk("SSC reset handled\n");
        }

        if (atomic_get(&stack_ready) && dust_gateway_rearm_pending()) {
            /* 一次会话结束后 Core 进入 Protected，主站下一次启动的 0x2100
             * 配置会被拒绝。这里在安全上下文重建网关（新的 boot_id、清空
             * 映射/判据），使连续多次启动不再需要断电或 ESC 复位。 */
            stack_stop();
            if (dust_gateway_rearm() != 0) {
                printk("Gateway rearm failed; IRQs remain disabled\n");
                return 0;
            }
            stack_enable();
        }

        dust_gateway_poll();
        MainLoop();
        /* MainLoop 期间可能产生新的输入映射，再 poll 一次以及时发布反馈。 */
        dust_gateway_poll();

        if (k_uptime_get() >= next_link_poll) {
            dust_esc_update_link();
            next_link_poll = k_uptime_get() + 100;
        }
        /* Active 时避免 100 us 系统 tick 取整形成额外周期延迟；ISR 仍可抢占。 */
        if (dust_gateway_active()) {
            k_busy_wait(10);
        } else {
            k_usleep(100);
        }
    }
}
