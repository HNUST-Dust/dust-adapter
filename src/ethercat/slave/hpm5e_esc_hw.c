/* SPDX-License-Identifier: Apache-2.0 */

#include "hpm5e_esc_hw.h"
#include "esc_dma.h"
#include "pdi_deferred.h"
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint32_t timer_origin_ms;
static unsigned int critical_key;
static unsigned int critical_depth;

uint16_t hpm5e_ssc_hw_init(void)
{
	return device_is_ready(esc) ? 0U : 1U;
}

void hpm5e_ssc_hw_release(void)
{
	hpm_esc_irq_enable(esc, false);
}

void hpm5e_ssc_esc_read(volatile void *data, uint16_t address, uint16_t length)
{
	int result = dust_esc_dma_read((void *)data, address, length);
	if (result == 1) {
		hpm_esc_read(esc, address, (void *)data, length);
	} else if (result < 0) {
		memset((void *)data, 0, length);
	}
}

void hpm5e_ssc_esc_write(const volatile void *data, uint16_t address, uint16_t length)
{
	if (dust_esc_dma_write((const void *)data, address, length) == 1) {
		hpm_esc_write(esc, address, (const void *)data, length);
	}
}

uint16_t hpm5e_ssc_al_event(void)
{
	uint16_t event;
	hpm_esc_read(esc, 0x0220U, &event, sizeof(event));
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
    // Independent Sync0 IRQ owns acknowledgement/service. Hide Sync0/1 AL
    // bits from SSC PDI_Isr so the same pulse cannot invoke Sync0 twice.
    event &= (uint16_t)~0x000cU;
#endif
    return event;
}

uint32_t hpm5e_ssc_timer_get(void)
{
	return k_uptime_get_32() - timer_origin_ms;
}

void hpm5e_ssc_timer_clear(void)
{
	timer_origin_ms = k_uptime_get_32();
}

uint16_t hpm5e_ssc_eeprom_reload(void)
{
	return hpm5e_ssc_process_eeprom() == 0 ? 0U : 1U;
}

void hpm5e_ssc_irq_enable(void)
{
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
    dust_pdi_enable();
#else
    irq_enable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
#endif
}

void hpm5e_ssc_irq_disable(void)
{
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
    dust_pdi_disable();
#else
    irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
#endif
}

void hpm5e_ssc_enter_critical(void)
{
	const unsigned int key = irq_lock();

	if (critical_depth++ == 0U) {
		critical_key = key;
	}
}

void hpm5e_ssc_leave_critical(void)
{
	if (critical_depth == 0U) {
		return;
	}
	if (--critical_depth == 0U) {
		irq_unlock(critical_key);
	}
}
