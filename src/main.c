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
#include "gateway.h"
#include "pdi_deferred.h"
#include "sync0_deferred.h"
#include "wait_profile.h"
#include "hpm5e31_phy.h"
#include "hpm5e_esc_hw.h"

#define SII_SIZE 2048U

extern unsigned char aEepromData[SII_SIZE];

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint8_t sii_image[SII_SIZE];
static atomic_t stack_ready;
static atomic_t reset_requested;

#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
static void sip_phy_prepare(void) {
    /* HPM5E00 SIP PHY guide v1.1: PHY0 MII is connected to PV internally. */
    const uint16_t mii_pads[] = {
        IOC_PAD_PV00, IOC_PAD_PV01, IOC_PAD_PV02, IOC_PAD_PV03, IOC_PAD_PV04,
        IOC_PAD_PV05, IOC_PAD_PV06, IOC_PAD_PV07, IOC_PAD_PV08, IOC_PAD_PV09,
        IOC_PAD_PV10, IOC_PAD_PV11, IOC_PAD_PV15,
    };
    for (size_t i = 0; i < ARRAY_SIZE(mii_pads); i++) {
        HPM_IOC->PAD[mii_pads[i]].FUNC_CTL =
            IOC_PAD_FUNC_CTL_ALT_SELECT_SET(11);
    }
    HPM_IOC->PAD[IOC_PAD_PW21].FUNC_CTL = IOC_PW21_FUNC_CTL_ESC0_REFCK;
    HPM_IOC->PAD[IOC_PAD_PV12].FUNC_CTL = IOC_PV12_FUNC_CTL_GPIO_V_12;
    gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DO_GPIOV, 12, 0);
    /* Unused PHY1 stays in reset; its analog pins are not available for UART.
     */
    HPM_IOC->PAD[IOC_PAD_PW12].FUNC_CTL = IOC_PW12_FUNC_CTL_GPIO_W_12;
    gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DO_GPIOW, 12, 0);
    const uint16_t analog_pads[] = {
        IOC_PAD_PA16, IOC_PAD_PA17, IOC_PAD_PA18, IOC_PAD_PA19, IOC_PAD_PA20,
        IOC_PAD_PA21, IOC_PAD_PA22, IOC_PAD_PA23, IOC_PAD_PA24, IOC_PAD_PA25,
        IOC_PAD_PA26, IOC_PAD_PA27, IOC_PAD_PA29, IOC_PAD_PW16, IOC_PAD_PW17,
    };
    for (size_t i = 0; i < ARRAY_SIZE(analog_pads); i++) {
        HPM_IOC->PAD[analog_pads[i]].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    }
    /* Address straps must be set by board resistors while reset is held.
     * Defer digital link mux and internal pulls until after reset settling. */
    HPM_IOC->PAD[IOC_PAD_PA28].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA28].PAD_CTL &= ~IOC_PAD_PAD_CTL_PE_MASK;
    HPM_IOC->PAD[IOC_PAD_PA29].PAD_CTL &= ~IOC_PAD_PAD_CTL_PE_MASK;
}

#endif

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
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
    dust_pdi_notify();
#else
    (void)hpm5e_ssc_process_eeprom();
    if (atomic_get(&stack_ready)) PDI_Isr();
#endif
}
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
static void service_pdi_owner(void) {
    (void)hpm5e_ssc_process_eeprom();
    // PDI_Isr invokes the application for any pending AL source once SM mode
    // is enabled. Only a real SM2 output event may trigger this owner cycle.
    if (atomic_get(&stack_ready) && bEscIntEnabled &&
        (hpm5e_ssc_al_event() & PROCESS_OUTPUT_EVENT)) {
        dust_pdi_cycle();
        PDI_Isr();
    }
}
#endif

#if defined(CONFIG_DUST_ECAT_DC_SYNC)
static void sync0_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(dev); ARG_UNUSED(user_data);
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    dust_wait_irq_enter();
#endif
    uint8_t acknowledged;
    // Acknowledge hardware immediately so subsequent pulses remain countable.
    hpm5e_ssc_esc_read(&acknowledged, 0x098e, 1);
    dust_sync0_notify();
}
static void service_sync0_owner(void) {
    if (bDcSyncActive && dust_gateway_active() && dust_sync0_error()) dust_gateway_sync_fault();
    if (atomic_get(&stack_ready)) Sync0_Isr();
    dust_sync0_set_active(dust_gateway_active());
}
#endif

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
#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
    sip_phy_prepare();
#endif
    hpm_esc_irq_enable(esc, false);
    hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_PDI, pdi_callback, NULL);
    hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_RESET, reset_callback, NULL);
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_SYNC0, sync0_callback, NULL);
#endif
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
#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
    /* ESC start enabled the shared 25 MHz reference; release SIP reset now. */
    k_msleep(100);
    printk(
        "SIP strap experiment: PA28/PA29 analog, internal pulls off; release "
        "PV12\n");
    gpio_write_pin(HPM_GPIO0, GPIO_DO_GPIOV, 12, 1);
    k_msleep(100);
    HPM_IOC->PAD[IOC_PAD_PA28].FUNC_CTL = IOC_PA28_FUNC_CTL_ESC0_CTR_1;
    HPM_IOC->PAD[IOC_PAD_PA28].PAD_CTL =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1);
    printk("SIP PHY0: PV12 released, PW21 REFCK, PA28 CTR1; PHY1 held reset\n");
#else
    printk(
        "HPM5E31: external RTL8201F/TJA1101B MII; "
        "PHY reset handled by board DTS\n");
#endif
    printk("SSC stage: entering MainInit (ESC IRQs masked)\n");

#if defined(CONFIG_DUST_ECAT_SM_SYNC)
    dust_pdi_reset();
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
    dust_sync0_reset();
#endif
#endif
    if (MainInit() != 0U) {
        printk("SSC MainInit failed\n");
        return 0;
    }
    if (dust_gateway_init() != 0) {
        printk("DTC1 gateway initialization failed; IRQs remain disabled\n");
        return 0;
    }
    atomic_set(&stack_ready, 1);
    hpm_esc_start(esc);
    /* Independent Sync0 is enabled only in the separately generated DC build. */
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    irq_enable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
#else
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
#endif
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
    printk(
        "DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
        "installed\n");
    int64_t next_link_poll = 0;

    for (;;) {
        if (atomic_cas(&reset_requested, 1, 0)) {
            atomic_clear(&stack_ready);
            hpm_esc_irq_enable(esc, false);
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
            dust_pdi_reset();
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
            dust_sync0_reset();
#endif
#endif
            if (MainInit() != 0U || dust_gateway_init() != 0) {
                printk("Gateway reset failed; IRQs remain disabled\n");
                return 0;
            }
            atomic_set(&stack_ready, 1);
            hpm_esc_start(esc);
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
            irq_enable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
#else
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
#endif
            irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
            printk("SSC reset handled\n");
        }
#if defined(CONFIG_DUST_ECAT_SM_SYNC)

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_PDI);
#endif
        (void)dust_pdi_service(service_pdi_owner);
#if defined(CONFIG_DUST_ECAT_DC_SYNC)

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_SYNC);
#endif
        dust_sync0_set_active(dust_gateway_active());
        (void)dust_sync0_service(service_sync0_owner);
        if (bDcSyncActive && dust_gateway_active() && dust_sync0_error()) dust_gateway_sync_fault();
#endif
#endif

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_POLL1);
#endif
        dust_gateway_poll();

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_SSC);
#endif
        MainLoop();

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_POLL2);
#endif
        dust_gateway_poll();
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
        /* Return to the normal PDI -> Sync0 owner entry before starting slow
         * MDIO maintenance. Never consume an event or run SSC from this check.
         * An IRQ after this snapshot can still wait for one maintenance step.
         */
        if (dust_pdi_pending() || dust_sync0_pending()) {
            continue;
        }
#endif
        if (k_uptime_get() >= next_link_poll) {

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_LINK);
#endif
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
            if (ecat_phy_update_link_step(esc)) {
                next_link_poll = k_uptime_get() + 100;
            }
#else
            ecat_phy_update_link(esc);
            next_link_poll = k_uptime_get() + 100;
#endif
        }
        // Active 时避免 100 us 系统 tick 取整形成额外周期延迟；ISR 仍可抢占。

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_IDLE);
#endif
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
        if (bEscIntEnabled) {

#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_phase(DUST_WAIT_SLEEP);
#endif
            dust_pdi_wait();
            continue;
        }
#endif
        if (dust_gateway_active()) {
            k_busy_wait(10);
        } else {
            k_usleep(100);
        }
    }
    return 0;
}
