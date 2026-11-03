/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ECAT_SII_H_
#define ECAT_SII_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSC SII 镜像大小（字节）。 */
#define SII_SIZE 2048U

/* 装载随固件提交的 SII 镜像；超出 SII_SIZE 的部分被忽略。 */
void sii_load(const uint8_t *image, size_t size);

/* EEPROM 模拟的 SII 读写与 reload 响应。addr 以 16 位字为单位。 */
int sii_read(uint32_t word_addr, uint16_t *data, size_t word_count);
int sii_write(uint32_t word_addr, uint16_t data);
int sii_reload(uint32_t *data);

#ifdef __cplusplus
}
#endif

#endif /* ECAT_SII_H_ */
