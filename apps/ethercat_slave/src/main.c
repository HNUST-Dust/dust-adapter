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
#include "hpm5e31_phy.h"
#include "hpm5e_esc_hw.h"

#define SII_SIZE 2048U

extern unsigned char aEepromData[SII_SIZE];

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint8_t sii_image[SII_SIZE];
static atomic_t stack_ready;
static atomic_t reset_requested;
static uint32_t phy_present;
static uint16_t previous_bmsr[32];

static void phy_config_dump(void) {
    ESC_Type *base = (ESC_Type *)DT_REG_ADDR(DT_NODELABEL(esc0));
    printk(
        "ESC PDI: type=%02x info=%04x cfg=%02x ext=%04x DL=%04x EEPROM=%04x "
        "active=%u invalid=%u\n",
        base->PDI_CTRL, base->PDI_INFO, base->PDI_CFG, base->PDI_EXT_CFG,
        base->ESC_DL_STAT, base->EEPROM_CTRL_STAT,
        ESC_PDI_INFO_PDIA_GET(base->PDI_INFO),
        ESC_PDI_INFO_PDICN_GET(base->PDI_INFO));
#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
    const uint16_t pads[] = {IOC_PAD_PW21, IOC_PAD_PV12, IOC_PAD_PW17,
                             IOC_PAD_PA27, IOC_PAD_PA28, IOC_PAD_PA29,
                             IOC_PAD_PA30, IOC_PAD_PA31, IOC_PAD_PA20,
                             IOC_PAD_PA21, IOC_PAD_PA22, IOC_PAD_PA23};
    const char *names[] = {"PW21", "PV12", "PW17", "PA27", "PA28", "PA29",
                           "PA30", "PA31", "PA20", "PA21", "PA22", "PA23"};
#else
    const uint16_t pads[] = {
        IOC_PAD_PA09, IOC_PAD_PA16, IOC_PAD_PA21, IOC_PAD_PA28, IOC_PAD_PA30,
        IOC_PAD_PA31, IOC_PAD_PF09, IOC_PAD_PF13, IOC_PAD_PF26, IOC_PAD_PF28};
    const char *names[] = {"PA09", "PA16", "PA21", "PA28", "PA30",
                           "PA31", "PF09", "PF13", "PF26", "PF28"};
#endif
    printk("ESC cfg: GPR0=%08x GPR1=%08x GPR2=%08x PHY0=%08x PHY1=%08x\n",
           base->GPR_CFG0, base->GPR_CFG1, base->GPR_CFG2, base->PHY_CFG0,
           base->PHY_CFG1);
#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
    printk("PHY reset: PV12 output=%u input=%u; PA28 level=%u\n",
           gpio_get_pin_output_status(HPM_GPIO0, GPIO_DO_GPIOV, 12),
           gpio_read_pin(HPM_GPIO0, GPIO_DI_GPIOV, 12),
           gpio_read_pin(HPM_GPIO0, GPIO_DI_GPIOA, 28));
#endif
    for (size_t i = 0; i < ARRAY_SIZE(pads); i++) {
        printk("IOC %s FUNC=%08x PAD=%08x\n", names[i],
               HPM_IOC->PAD[pads[i]].FUNC_CTL, HPM_IOC->PAD[pads[i]].PAD_CTL);
    }
}

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

/* The SDK takes PDI ownership for each MDIO read. Always return it to ESC,
 * including failed reads, so autonomous link monitoring can continue. */
static hpm_stat_t phy_read(uint8_t address, uint8_t reg, uint16_t *value) {
    ESC_Type *base = (ESC_Type *)DT_REG_ADDR(DT_NODELABEL(esc0));
    hpm_stat_t rc = hpm_esc_mdio_read(
        esc, (uint8_t)(ESC_PHY_ADDR_SHOW_MASK | address), reg, value);
    base->MIIM_PDI_ACC_STAT = 0;
    return rc;
}

static void phy_scan(void) {
    printk("PHY diagnostic: offset=%u; scanning MDIO 0..31\n",
           DT_PROP(DT_NODELABEL(esc0), phy_addr_offset));
    phy_present = 0;
    for (uint8_t address = 0; address < 32; address++) {
        uint16_t id1 = 0xffff, id2 = 0xffff;
        hpm_stat_t rc1 = phy_read(address, 2, &id1);
        hpm_stat_t rc2 = phy_read(address, 3, &id2);
        printk("PHY addr=%u ID=%04x:%04x rc=%ld/%ld\n", address, id1, id2,
               (long)rc1, (long)rc2);
        uint16_t bmcr = 0xffff, bmsr = 0xffff;
        hpm_stat_t rc0 = phy_read(address, 0, &bmcr);
        hpm_stat_t rcs = phy_read(address, 1, &bmsr);
        printk("PHY addr=%u BMCR=%04x BMSR=%04x rc=%ld/%ld\n", address, bmcr,
               bmsr, (long)rc0, (long)rcs);
        if (rc0 != status_success || rcs != status_success || bmcr == 0xffff ||
            bmsr == 0xffff || (bmcr == 0 && bmsr == 0)) {
            continue;
        }
        phy_present |= BIT(address);
        previous_bmsr[address] = 0xffff;
        for (uint8_t reg = 0; reg <= 6; reg++) {
            uint16_t value = 0xffff;
            hpm_stat_t rc = phy_read(address, reg, &value);
            printk("PHY addr=%u reg=%u value=%04x rc=%ld\n", address, reg,
                   value, (long)rc);
        }
        /* Give the RTT reader time to consume the startup dump. */
        k_msleep(20);
    }
    printk("PHY scan complete mask=%08x (multiple addresses may be aliases)\n",
           phy_present);
}

static void phy_poll(bool heartbeat) {
    for (uint8_t address = 0; address < 32; address++) {
        if (!(phy_present & BIT(address))) {
            continue;
        }
        uint16_t bmcr = 0, bmsr = 0;
        /* BMSR link-down is latched; read twice for the current state. */
        hpm_stat_t rc = phy_read(address, 1, &bmsr);
        if (rc == status_success) {
            rc = phy_read(address, 1, &bmsr);
        }
        if (rc != status_success ||
            phy_read(address, 0, &bmcr) != status_success) {
            printk("PHY addr=%u read failed\n", address);
            continue;
        }
        if (heartbeat || bmsr != previous_bmsr[address]) {
            printk(
                "PHY t=%lld addr=%u BMCR=%04x BMSR=%04x link=%u "
                "reset=%u powerdown=%u isolate=%u AN=%u ANdone=%u "
                "speed_bit100=%u duplex_bit=%u\n",
                (long long)k_uptime_get(), address, bmcr, bmsr,
                !!(bmsr & BIT(2)), !!(bmcr & BIT(15)), !!(bmcr & BIT(11)),
                !!(bmcr & BIT(10)), !!(bmcr & BIT(12)), !!(bmsr & BIT(5)),
                !!(bmcr & BIT(13)), !!(bmcr & BIT(8)));
        }
        previous_bmsr[address] = bmsr;
    }
}

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
    if (atomic_get(&stack_ready)) {
        PDI_Isr();
    }
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
#if defined(CONFIG_BOARD_DUST_HPM5E3Y)
    sip_phy_prepare();
#endif
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
    phy_config_dump();
    phy_scan();
    phy_poll(true);
    printk("SSC stage: entering MainInit (ESC IRQs masked)\n");

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
    /* FreeRun 仅使用 PDI/RESET；没有 SSC 回调处理 SYNC0/1。 */
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
    printk(
        "DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
        "installed\n");
    int64_t next_phy_poll = 0;
    unsigned int poll_count = 0;
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
        MainLoop();
        dust_gateway_poll();
        if (k_uptime_get() >= next_link_poll) {
            ecat_phy_update_link(esc);
            next_link_poll = k_uptime_get() + 100;
        }
        if (!dust_gateway_active() && k_uptime_get() >= next_phy_poll) {
            phy_poll((poll_count++ % 10U) == 0U);
            next_phy_poll = k_uptime_get() + 1000;
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
