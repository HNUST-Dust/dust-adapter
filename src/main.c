/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "drivers/buzzer.h"
#include "ecat/runtime.h"

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
extern int dust_imu_heater_init(void);
#endif

int main(void) {
    if (dust_ecat_runtime_init() != 0) {
        return 0;
    }

#if defined(CONFIG_DUST_BUZZER)
    const int buzzer_rc = dust_buzzer_init();
    if (buzzer_rc) {
        printk("Buzzer init failed: %d\n", buzzer_rc);
    }
#endif

    const int start_rc = dust_ecat_runtime_start();
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

    dust_ecat_runtime_enable();
    printk("DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
           "installed\n");

    for (;;) {
        const dust_ecat_step_t step = dust_ecat_runtime_step();
        if (step == DUST_ECAT_STEP_FATAL) {
            return 0;
        }
        /* Active 时避免 100 us 系统 tick 取整形成额外周期延迟；ISR 仍可抢占。 */
        if (step == DUST_ECAT_STEP_ACTIVE) {
            k_busy_wait(10);
        } else {
            k_usleep(100);
        }
    }
}
