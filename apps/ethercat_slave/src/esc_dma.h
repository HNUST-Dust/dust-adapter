/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(CONFIG_DUST_ECAT_ESC_DMA)
int dust_esc_dma_init(void);
int dust_esc_dma_error(void);
/* 0 completed, 1 use CPU (not started), negative sticky transport error. */
int dust_esc_dma_read(void *data, uint16_t address, uint16_t length);
int dust_esc_dma_write(const void *data, uint16_t address, uint16_t length);
#else
static inline int dust_esc_dma_init(void) {
    return 0;
}
static inline int dust_esc_dma_error(void) {
    return 0;
}
static inline int dust_esc_dma_read(void *p, uint16_t a, uint16_t n) {
    (void)p;
    (void)a;
    (void)n;
    return 1;
}
static inline int dust_esc_dma_write(const void *p, uint16_t a, uint16_t n) {
    (void)p;
    (void)a;
    (void)n;
    return 1;
}
#endif
#ifdef __cplusplus
}
#endif
