/* SPDX-License-Identifier: Apache-2.0 */

#ifndef DUST_ECAT_SII_H_
#define DUST_ECAT_SII_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSC SII 镜像大小（字节）。 */
#define DUST_SII_SIZE 2048U

/* 装载随固件提交的 SII 镜像；超出 DUST_SII_SIZE 的部分被忽略。 */
void dust_sii_load(const uint8_t *image, size_t size);

/* EEPROM 模拟的 SII 读写与 reload 响应。addr 以 16 位字为单位。 */
int dust_sii_read(uint32_t word_addr, uint16_t *data, size_t word_count);
int dust_sii_write(uint32_t word_addr, uint16_t data);
int dust_sii_reload(uint32_t *data);

#ifdef __cplusplus
}
#endif

#endif /* DUST_ECAT_SII_H_ */
