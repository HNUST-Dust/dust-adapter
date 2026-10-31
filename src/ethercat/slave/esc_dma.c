/* SPDX-License-Identifier: Apache-2.0 */
#include "esc_dma.h"
#if defined(CONFIG_DUST_ECAT_ESC_DMA)
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/ethercat/hpm_esc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#define DMA_NODE DT_PATH(zephyr_user)
#define DMA_DEV DT_DMAS_CTLR_BY_NAME(DMA_NODE, esc)
#define DMA_CHANNEL DT_DMAS_CELL_BY_NAME(DMA_NODE, esc, channel)
#define DMA_SLOT DT_DMAS_CELL_BY_NAME(DMA_NODE, esc, source)
#define ASSERT_CHANNEL(node, prop, idx)                                   \
    BUILD_ASSERT(                                                         \
        DT_SAME_NODE(node, DMA_NODE) ||                                   \
            !DT_SAME_NODE(DT_PHANDLE_BY_IDX(node, prop, idx), DMA_DEV) || \
            DT_PHA_BY_IDX(node, prop, idx, channel) != DMA_CHANNEL,       \
        "ESC DMA conflicts with another device");
#define CHECK_NODE(node)                      \
    COND_CODE_1(DT_NODE_HAS_PROP(node, dmas), \
                (DT_FOREACH_PROP_ELEM(node, dmas, ASSERT_CHANNEL)), ())
DT_FOREACH_STATUS_OKAY_NODE(CHECK_NODE)
BUILD_ASSERT(DMA_CHANNEL < DT_PROP(DMA_DEV, dma_channels));
BUILD_ASSERT(DUST_RAM_END <= 0xfe00U);
static const struct device *const dma = DEVICE_DT_GET(DMA_DEV);
static const struct device *const esc = DEVICE_DT_GET(DT_NODELABEL(esc0));
/* Exported for post-run JTAG inspection. ABI: 16 little-endian u32 values. */
struct DmaStats {
    uint32_t version, ready, selftest, reads, writes, cpu_fallbacks,
        irq_fallbacks;
    uint32_t config_errors, start_errors, completion_errors, timeouts,
        reentries;
    int32_t last_error;
    uint32_t channel, burst, max_wait_cycles;
};
struct DmaStats dust_esc_dma_stats = {
    .version = 1, .channel = DMA_CHANNEL, .burst = 1};
static __nocache __aligned(32) uint8_t bounce[MAX_PD_INPUT_SIZE];
static atomic_t done, completion, busy, fault;
static bool ready;
static void Complete(const struct device *dev, void *user, uint32_t channel,
                     int status) {
    (void)dev;
    (void)user;
    (void)channel;
    atomic_set(&completion, status);
    atomic_set(&done, 1);
}
static int Fail(int error) {
    if (error >= 0) {
        error = -EIO;
    }
    atomic_set(&fault, error);
    dust_esc_dma_stats.last_error = error;
    (void)dma_stop(dma, DMA_CHANNEL);
    atomic_clear(&busy);
    return error;
}
static int Copy(uint32_t source, uint32_t destination, uint16_t length) {
    struct dma_block_config block = {0};
    struct dma_config config = {0};
    block.source_address = source;
    block.dest_address = destination;
    block.block_size = length;
    block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
    block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
    config.channel_direction = MEMORY_TO_MEMORY;
    config.dma_slot = DMA_SLOT;
    config.source_data_size = 4;
    config.dest_data_size = 4;
    config.source_burst_length = 1;
    config.dest_burst_length = 1;
    config.block_count = 1;
    config.head_block = &block;
    config.dma_callback = Complete;
    atomic_clear(&done);
    atomic_clear(&completion);
    int result = dma_config(dma, DMA_CHANNEL, &config);
    if (result) {
        ++dust_esc_dma_stats.config_errors;
        return Fail(result);
    }
    const uint64_t start = k_cycle_get_64();
    result = dma_start(dma, DMA_CHANNEL);
    if (result) {
        ++dust_esc_dma_stats.start_errors;
        return Fail(result);
    }
    while (!atomic_get(&done)) {
        if (k_cyc_to_us_floor64(k_cycle_get_64() - start) >= 500U) {
            ++dust_esc_dma_stats.timeouts;
            return Fail(-ETIMEDOUT);
        }
    }
    uint32_t elapsed = (uint32_t)(k_cycle_get_64() - start);
    if (elapsed > dust_esc_dma_stats.max_wait_cycles) {
        dust_esc_dma_stats.max_wait_cycles = elapsed;
    }
    result = (int)atomic_get(&completion);
    if (result) {
        ++dust_esc_dma_stats.completion_errors;
        return Fail(result);
    }
    return 0;
}
int dust_esc_dma_error(void) {
    return (int)atomic_get(&fault);
}
int dust_esc_dma_init(void) {
    if (ready) {
        return 0;
    }
    if (!device_is_ready(dma)) {
        return Fail(-ENODEV);
    }
    /* Preserve an unused PDRAM scratch span; exercise both directions before
     * OP. */
    uint8_t saved[512], pattern[512], readback[512];
    hpm_esc_read(esc, 0xfe00, saved, sizeof(saved));
    for (unsigned i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)(i * 37U + 11U);
    }
    memcpy(bounce, pattern, sizeof(pattern));
    int result = Copy((uint32_t)(uintptr_t)bounce,
                      (uint32_t)hpm_esc_base(esc) + 0xfe00, sizeof(pattern));
    if (!result) {
        hpm_esc_read(esc, 0xfe00, readback, sizeof(readback));
        if (memcmp(pattern, readback, sizeof(pattern))) {
            result = Fail(-EILSEQ);
        }
    }
    if (!result) {
        memset(bounce, 0, sizeof(pattern));
        result = Copy((uint32_t)hpm_esc_base(esc) + 0xfe00,
                      (uint32_t)(uintptr_t)bounce, sizeof(pattern));
        if (!result && memcmp(pattern, bounce, sizeof(pattern))) {
            result = Fail(-EILSEQ);
        }
    }
    /* Never race a possibly incomplete/aborted DMA by retrying with the CPU. */
    if (result) {
        return result;
    }
    hpm_esc_write(esc, 0xfe00, saved, sizeof(saved));
    ready = true;
    dust_esc_dma_stats.ready = 1;
    dust_esc_dma_stats.selftest = 1;
    return 0;
}
static int Transfer(void *data, uint16_t address, uint16_t length,
                    bool output) {
    int error = dust_esc_dma_error();
    if (error) {
        return error;
    }
    if (!ready || address != (output ? DUST_TX_ADDRESS : DUST_RX_ADDRESS) ||
        length < 64U || length > sizeof(bounce) || (length & 3U)) {
        ++dust_esc_dma_stats.cpu_fallbacks;
        return 1;
    }
    unsigned key = irq_lock();
    bool interrupts_enabled = arch_irq_unlocked(key);
    irq_unlock(key);
    /* The Zephyr DMA completion is delivered by IRQ. Never wait with IRQs
     * masked. */
    if (k_is_in_isr() || !interrupts_enabled) {
        ++dust_esc_dma_stats.irq_fallbacks;
        return 1;
    }
    if (!atomic_cas(&busy, 0, 1)) {
        ++dust_esc_dma_stats.reentries;
        return Fail(-EBUSY);
    }
    if (output) {
        memcpy(bounce, data, length);
    }
    uint32_t ram = (uint32_t)(uintptr_t)bounce,
             process = (uint32_t)hpm_esc_base(esc) + address;
    int result = Copy(output ? ram : process, output ? process : ram, length);
    if (result) {
        return result;
    }
    if (!output) {
        memcpy(data, bounce, length);
    }
    if (output) {
        ++dust_esc_dma_stats.writes;
    } else {
        ++dust_esc_dma_stats.reads;
    }
    atomic_clear(&busy);
    return 0;
}
int dust_esc_dma_read(void *data, uint16_t address, uint16_t length) {
    return Transfer(data, address, length, false);
}
int dust_esc_dma_write(const void *data, uint16_t address, uint16_t length) {
    return Transfer((void *)data, address, length, true);
}
#endif
