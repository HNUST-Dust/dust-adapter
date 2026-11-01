/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <hpm_esc_drv.h>
#include <hpm_gpio_drv.h>
#include <hpm_iomux.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ethercat/hpm_esc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "applInterface.h"
#include "ecatappl.h"
#include "ecat/gateway.h"
#include "drivers/buzzer.h"
#include "ecat/hardware_interfaces/pdi_deferred.h"
#include "hpm5e31_phy.h"
#include "ecat/hardware_interfaces/hpm5e_esc_hw.h"

#if defined(CONFIG_DUST_BOARD_OUTPUTS)
extern int dust_imu_heater_init(void);
#endif

#define SII_SIZE 2048U

extern unsigned char aEepromData[SII_SIZE];

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint8_t sii_image[SII_SIZE];
static atomic_t stack_ready;
static atomic_t reset_requested;

static int sii_read(uint32_t word_addr, uint16_t *data, size_t word_count,
                    void *user_data) {
    uint8_t *image = user_data;
    const size_t image_words = SII_SIZE / sizeof(uint16_t);
    if (word_addr > image_words || word_count > image_words - word_addr) {
        return -EINVAL;
    }
    const size_t offset = (size_t)word_addr * sizeof(uint16_t);
    const size_t length = word_count * sizeof(uint16_t);
    memcpy(data, image + offset, length);
    return 0;
}

static int sii_write(uint32_t word_addr, uint16_t data, void *user_data) {
    uint8_t *image = user_data;
    if (word_addr >= SII_SIZE / sizeof(uint16_t)) {
        return -EINVAL;
    }
    const size_t offset = (size_t)word_addr * sizeof(uint16_t);
    memcpy(image + offset, &data, sizeof(data));
    return 0;
}

static int sii_reload(uint32_t *data, void *user_data) {
    uint8_t *image = user_data;
    /* HPM ESC reload response: alias in bits 15:0, enhanced link
     * detection and port enables in bits 20:16 (not raw SII word 0/1). */
    const uint8_t cfg = image[1];
    *data =
        (uint32_t)image[8] | ((uint32_t)image[9] << 8) |
        (ESC_ESC_CFG_ELDAP_GET(cfg) << 16) | (ESC_ESC_CFG_ELP0_GET(cfg) << 17) |
        (ESC_ESC_CFG_ELP1_GET(cfg) << 18) | (ESC_ESC_CFG_ELP2_GET(cfg) << 19) |
        (ESC_ESC_CFG_ELP3_GET(cfg) << 20);
    printk("SII reload: config=%02x%02x response=%08x\n", image[1], image[0],
           *data);
    return 0;
}

/* SII 镜像由应用持有；新驱动仅提供 ESC 寄存器访问。 */
int hpm5e_ssc_process_eeprom(void) {
    ESC_Type *base = (ESC_Type *)hpm_esc_base(esc);
    if ((base->EEPROM_CTRL_STAT & ESC_EEPROM_CTRL_STAT_BUSY_MASK) == 0U) {
        return 0;
    }
    const uint8_t command = esc_get_eeprom_cmd(base);
    const uint32_t address = esc_get_eeprom_word_address(base);
    int ret;

    switch (command) {
        case esc_eeprom_read_cmd: {
            uint16_t words[2] = {0U};
            ret = sii_read(address, words, ARRAY_SIZE(words), sii_image);
            if (ret == 0) {
                esc_write_eeprom_data(
                    base, (uint64_t)words[0] | ((uint64_t)words[1] << 16));
            }
            break;
        }
        case esc_eeprom_write_cmd:
            ret = sii_write(address, (uint16_t)esc_read_eeprom_data(base),
                            sii_image);
            break;
        case esc_eeprom_reload_cmd: {
            uint32_t value;
            ret = sii_reload(&value, sii_image);
            if (ret == 0) {
                esc_write_eeprom_data(base, value);
            }
            break;
        }
        default:
            return 0;
    }
    esc_eeprom_emulation_ack(base, command, ret != 0, false);
    return ret;
}

static void pdi_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    (void)hpm5e_ssc_process_eeprom();
    if (atomic_get(&stack_ready)) PDI_Isr();
}


static void reset_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    atomic_set(&reset_requested, 1);
}

int main(void) {
    if (!device_is_ready(esc)) {
        printk("EtherCAT ESC device is not ready\n");
        return 0;
    }

    memcpy(sii_image, aEepromData, sizeof(sii_image));
    hpm_esc_irq_enable(esc, false);
    hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_PDI, pdi_callback, NULL);
    hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_RESET, reset_callback, NULL);
    hpm_esc_configure_eeprom(esc, true, false);
    if (hpm5e_ssc_process_eeprom() != 0) {
        printk("SII startup response failed\n");
        return 0;
    }
    const int eeprom_status = hpm_esc_check_eeprom(esc);
    if (eeprom_status != 0) {
        printk("SII loading status=%d\n", eeprom_status);
        if (eeprom_status != -EBADMSG) {
            return 0;
        }
    }
    ecat_phy_reset(esc);
    hpm_esc_configure_phy(esc);
    if (ecat_phy_config(esc) != status_success) {
        printk("EtherCAT PHY initialization failed\n");
        return 0;
    }
    hpm_esc_configure_signals(esc);
    ecat_phy_update_link(esc);
    printk(
        "HPM5E31: external RTL8201F/TJA1101B MII; "
        "PHY reset handled by board DTS\n");

    #if defined(CONFIG_DUST_BUZZER)
    int buzzer_rc = dust_buzzer_init();
    if (buzzer_rc) printk("Buzzer init failed: %d\n", buzzer_rc);
    #endif

    if (MainInit() != 0U) {
        printk("SSC MainInit failed\n");
        return 0;
    }
    if (dust_gateway_init() != 0) {
        printk("DTC1 gateway initialization failed; IRQs remain disabled\n");
        return 0;
    }
    #if defined(CONFIG_DUST_BOARD_OUTPUTS)
    /* Start the PID only after the IMU is sampling, so a slow first sample
     * cannot latch the heater fault. */
    int heater_rc = dust_imu_heater_init();
    if (heater_rc) printk("IMU heater init failed: %d\n", heater_rc);
    #endif
    atomic_set(&stack_ready, 1);
    hpm_esc_start(esc);
    /* FreeRun leaves the ESC synchronization interrupts disabled. */
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
    printk(
        "DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
        "installed\n");
    int64_t next_link_poll = 0;

    for (;;) {
        if (atomic_cas(&reset_requested, 1, 0)) {
            atomic_clear(&stack_ready);
            hpm_esc_irq_enable(esc, false);
            if (MainInit() != 0U || dust_gateway_init() != 0) {
                printk("Gateway reset failed; IRQs remain disabled\n");
                return 0;
            }
            atomic_set(&stack_ready, 1);
            hpm_esc_start(esc);
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
            printk("SSC reset handled\n");
        }

        if (atomic_get(&stack_ready) && dust_gateway_rearm_pending()) {
            /* 一次会话结束后 Core 进入 Protected，主站下一次启动的 0x2100
             * 配置会被拒绝。这里在安全上下文重建网关（新的 boot_id、清空
             * 映射/判据），使连续多次启动不再需要断电或 ESC 复位。 */
            atomic_clear(&stack_ready);
            hpm_esc_irq_enable(esc, false);
            if (dust_gateway_rearm() != 0) {
                printk("Gateway rearm failed; IRQs remain disabled\n");
                return 0;
            }
            atomic_set(&stack_ready, 1);
            hpm_esc_start(esc);
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
        }

        dust_gateway_poll();

        MainLoop();

        dust_gateway_poll();
        if (k_uptime_get() >= next_link_poll) {

            ecat_phy_update_link(esc);
            next_link_poll = k_uptime_get() + 100;
        }
        // Active 时避免 100 us 系统 tick 取整形成额外周期延迟；ISR 仍可抢占。

        if (dust_gateway_active()) {
            k_busy_wait(10);
        } else {
            k_usleep(100);
        }
    }
    return 0;
}
