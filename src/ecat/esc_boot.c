/* SPDX-License-Identifier: Apache-2.0 */

/* ESC/PHY/SII 启动策略、FreeRun 启停与中断钩子；不含 SSC 生命周期。 */

#include "ecat/esc_hw.h"

#include <errno.h>

#include <hpm_esc_drv.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ecat/sii.h"
#include "hpm5e31_phy.h"

extern unsigned char aEepromData[DUST_SII_SIZE];

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static void (*pdi_hook)(void);
static void (*reset_hook)(void);

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
	dust_sii_load(aEepromData, DUST_SII_SIZE);
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
