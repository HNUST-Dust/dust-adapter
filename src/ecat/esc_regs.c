/* SPDX-License-Identifier: Apache-2.0 */

/* ESC 寄存器原语与 EEPROM 命令循环；不包含 PHY/启动策略。 */

#include "ecat/esc_hw.h"

#include <hpm_esc_drv.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ecat/sii.h"

static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
static uint32_t timer_origin_ms;
static unsigned int critical_key;
static unsigned int critical_depth;

uint16_t dust_esc_hw_init(void)
{
	return device_is_ready(esc) ? 0U : 1U;
}

void dust_esc_hw_release(void)
{
	hpm_esc_irq_enable(esc, false);
}

/* ESC 复位/启动后由主循环或 PDI ISR 调用，响应 ESC 的 EEPROM 模拟命令。 */
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
		ret = dust_sii_read(address, words, ARRAY_SIZE(words));
		if (ret == 0) {
			esc_write_eeprom_data(
			    base, (uint64_t)words[0] | ((uint64_t)words[1] << 16));
		}
		break;
	}
	case esc_eeprom_write_cmd:
		ret = dust_sii_write(address, (uint16_t)esc_read_eeprom_data(base));
		break;
	case esc_eeprom_reload_cmd: {
		uint32_t value;
		ret = dust_sii_reload(&value);
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
