/* SPDX-License-Identifier: Apache-2.0 */

#include "hpm5e_esc_hw.h"
#include "rtt_trace.h"
#include "esc_dma.h"
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
	dust_trace_read(DUST_TR_ESC_READ_BEGIN, address, length);
	int result = dust_esc_dma_read((void *)data, address, length);
	if (result == 1) {
		hpm_esc_read(esc, address, (void *)data, length);
	} else if (result < 0) {
		memset((void *)data, 0, length);
	}
	dust_trace_read(DUST_TR_ESC_READ_END, address, length);
}

void hpm5e_ssc_esc_write(const volatile void *data, uint16_t address, uint16_t length)
{
	dust_trace_esc(DUST_TR_ESC_BEGIN, address, length);
	if (dust_esc_dma_write((const void *)data, address, length) == 1) {
		hpm_esc_write(esc, address, (const void *)data, length);
	}
	dust_trace_esc(DUST_TR_ESC_END, address, length);
}

uint16_t hpm5e_ssc_al_event(void)
{
	uint16_t event;
	hpm_esc_read(esc, 0x0220U, &event, sizeof(event));
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
	irq_enable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
}

void hpm5e_ssc_irq_disable(void)
{
	irq_disable(DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0));
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
