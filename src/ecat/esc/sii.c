/* SPDX-License-Identifier: Apache-2.0 */

#include "ecat/esc/sii.h"

#include <errno.h>
#include <string.h>

#include <hpm_esc_drv.h>
#include <zephyr/sys/printk.h>

static uint8_t sii_image[DUST_SII_SIZE];

void dust_sii_load(const uint8_t *image, size_t size)
{
	if (size > DUST_SII_SIZE) {
		size = DUST_SII_SIZE;
	}
	memcpy(sii_image, image, size);
}

int dust_sii_read(uint32_t word_addr, uint16_t *data, size_t word_count)
{
	const size_t image_words = DUST_SII_SIZE / sizeof(uint16_t);
	if (word_addr > image_words || word_count > image_words - word_addr) {
		return -EINVAL;
	}
	const size_t offset = (size_t)word_addr * sizeof(uint16_t);
	memcpy(data, sii_image + offset, word_count * sizeof(uint16_t));
	return 0;
}

int dust_sii_write(uint32_t word_addr, uint16_t data)
{
	if (word_addr >= DUST_SII_SIZE / sizeof(uint16_t)) {
		return -EINVAL;
	}
	memcpy(sii_image + (size_t)word_addr * sizeof(uint16_t), &data,
	       sizeof(data));
	return 0;
}

int dust_sii_reload(uint32_t *data)
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
