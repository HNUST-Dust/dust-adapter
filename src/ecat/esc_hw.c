/* SPDX-License-Identifier: Apache-2.0 */

#include "ecat/esc_hw.h"

#include <errno.h>
#include <string.h>

#include <hpm_esc_drv.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "hpm5e31_phy.h"

#define SII_SIZE 2048U

extern unsigned char aEepromData[SII_SIZE];

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint32_t timer_origin_ms;
static unsigned int critical_key;
static unsigned int critical_depth;
static uint8_t sii_image[SII_SIZE];
static void (*pdi_hook)(void);
static void (*reset_hook)(void);

uint16_t dust_esc_hw_init(void)
{
	return device_is_ready(esc) ? 0U : 1U;
}

void dust_esc_hw_release(void)
{
	hpm_esc_irq_enable(esc, false);
}

static int sii_read(uint32_t word_addr, uint16_t *data, size_t word_count)
{
	const size_t image_words = SII_SIZE / sizeof(uint16_t);
	if (word_addr > image_words || word_count > image_words - word_addr) {
		return -EINVAL;
	}
	const size_t offset = (size_t)word_addr * sizeof(uint16_t);
	memcpy(data, sii_image + offset, word_count * sizeof(uint16_t));
	return 0;
}

static int sii_write(uint32_t word_addr, uint16_t data)
{
	if (word_addr >= SII_SIZE / sizeof(uint16_t)) {
		return -EINVAL;
	}
	memcpy(sii_image + (size_t)word_addr * sizeof(uint16_t), &data,
	       sizeof(data));
	return 0;
}

static int sii_reload(uint32_t *data)
{
	/* HPM ESC reload response: alias in bits 15:0, enhanced link
	 * detection and port enables in bits 20:16 (not raw SII word 0/1). */
	const uint8_t cfg = sii_image[1];
	*data = (uint32_t)sii_image[8] | ((uint32_t)sii_image[9] << 8) |
	        (ESC_ESC_CFG_ELDAP_GET(cfg) << 16) |
	        (ESC_ESC_CFG_ELP0_GET(cfg) << 17) |
	        (ESC_ESC_CFG_ELP1_GET(cfg) << 18) |
	        (ESC_ESC_CFG_ELP2_GET(cfg) << 19) |
	        (ESC_ESC_CFG_ELP3_GET(cfg) << 20);
	printk("SII reload: config=%02x%02x response=%08x\n", sii_image[1],
	       sii_image[0], *data);
	return 0;
}

/* SII 镜像由应用持有；新驱动仅提供 ESC 寄存器访问。 */
int dust_esc_process_eeprom(void)
{
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
		ret = sii_read(address, words, ARRAY_SIZE(words));
		if (ret == 0) {
			esc_write_eeprom_data(
			    base, (uint64_t)words[0] | ((uint64_t)words[1] << 16));
		}
		break;
	}
	case esc_eeprom_write_cmd:
		ret = sii_write(address, (uint16_t)esc_read_eeprom_data(base));
		break;
	case esc_eeprom_reload_cmd: {
		uint32_t value;
		ret = sii_reload(&value);
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

static void esc_pdi_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	(void)dust_esc_process_eeprom();
	if (pdi_hook) {
		pdi_hook();
	}
}

static void esc_reset_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	if (reset_hook) {
		reset_hook();
	}
}

void dust_esc_set_pdi_hook(void (*hook)(void))
{
	pdi_hook = hook;
}

void dust_esc_set_reset_hook(void (*hook)(void))
{
	reset_hook = hook;
}

int dust_esc_init(void)
{
	if (!device_is_ready(esc)) {
		printk("EtherCAT ESC device is not ready\n");
		return -ENODEV;
	}
	memcpy(sii_image, aEepromData, sizeof(sii_image));
	hpm_esc_irq_enable(esc, false);
	hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_PDI, esc_pdi_callback, NULL);
	hpm_esc_set_irq_callback(esc, HPM_ESC_IRQ_RESET, esc_reset_callback, NULL);
	hpm_esc_configure_eeprom(esc, true, false);
	if (dust_esc_process_eeprom() != 0) {
		printk("SII startup response failed\n");
		return -EIO;
	}
	const int eeprom_status = hpm_esc_check_eeprom(esc);
	if (eeprom_status != 0) {
		printk("SII loading status=%d\n", eeprom_status);
		if (eeprom_status != -EBADMSG) {
			return eeprom_status;
		}
	}
	ecat_phy_reset(esc);
	hpm_esc_configure_phy(esc);
	if (ecat_phy_config(esc) != status_success) {
		printk("EtherCAT PHY initialization failed\n");
		return -EIO;
	}
	hpm_esc_configure_signals(esc);
	ecat_phy_update_link(esc);
	printk("HPM5E31: external RTL8201F/TJA1101B MII; "
	       "PHY reset handled by board DTS\n");
	return 0;
}

void dust_esc_stop(void)
{
	hpm_esc_irq_enable(esc, false);
}

void dust_esc_start(void)
{
	hpm_esc_start(esc);
	/* FreeRun leaves the ESC synchronization interrupts disabled. */
	irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 1));
	irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 2));
}

void dust_esc_update_link(void)
{
	ecat_phy_update_link(esc);
}

void dust_esc_esc_read(volatile void *data, uint16_t address, uint16_t length)
{
	hpm_esc_read(esc, address, (void *)data, length);
}

void dust_esc_esc_write(const volatile void *data, uint16_t address, uint16_t length)
{
	hpm_esc_write(esc, address, (const void *)data, length);
}

uint16_t dust_esc_al_event(void)
{
	uint16_t event;
	hpm_esc_read(esc, 0x0220U, &event, sizeof(event));
    return event;
}

uint32_t dust_esc_timer_get(void)
{
	return k_uptime_get_32() - timer_origin_ms;
}

void dust_esc_timer_clear(void)
{
	timer_origin_ms = k_uptime_get_32();
}

uint16_t dust_esc_eeprom_reload(void)
{
	return dust_esc_process_eeprom() == 0 ? 0U : 1U;
}

void dust_esc_irq_enable(void)
{
    irq_enable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
}

void dust_esc_irq_disable(void)
{
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
}

void dust_esc_enter_critical(void)
{
	const unsigned int key = irq_lock();

	if (critical_depth++ == 0U) {
		critical_key = key;
	}
}

void dust_esc_leave_critical(void)
{
	if (critical_depth == 0U) {
		return;
	}
	if (--critical_depth == 0U) {
		irq_unlock(critical_key);
	}
}
