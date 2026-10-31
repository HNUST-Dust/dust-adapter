/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DMA_CHANNEL 6
#define DMA_SLOT 0
#define MAX_PD_INPUT_SIZE 1536
#define DUST_TX_ADDRESS 0x1d00
#define DUST_RX_ADDRESS 0x1100
#define __nocache
#define __aligned(n) __attribute__((aligned(n)))
#define MEMORY_TO_MEMORY 0
#define DMA_ADDR_ADJ_INCREMENT 0
struct device {
    int dummy;
};
static struct device mock;
static const struct device *const dma = &mock, *const esc = &mock;
typedef int atomic_t;
static int atomic_get(atomic_t *p) {
    return *p;
}
static void atomic_set(atomic_t *p, int n) {
    *p = n;
}
static void atomic_clear(atomic_t *p) {
    *p = 0;
}
static bool atomic_cas(atomic_t *p, int old, int next) {
    if (*p != old) {
        return false;
    }
    *p = next;
    return true;
}
struct dma_block_config {
    uint32_t source_address, dest_address, block_size, source_addr_adj,
        dest_addr_adj;
};
struct dma_config {
    uint32_t channel_direction, dma_slot, source_data_size, dest_data_size,
        source_burst_length, dest_burst_length, block_count;
    struct dma_block_config *head_block;
    void (*dma_callback)(const struct device *, void *, uint32_t, int);
};
static uint8_t process_ram[65536];
static uint64_t clock_cycles;
static int mode, stops, starts, configs;
static bool in_isr, interrupts_enabled = true, device_ready = true;
static struct dma_block_config current;
static void (*callback)(const struct device *, void *, uint32_t, int);
static void Perform(void);
static int device_is_ready(const struct device *d) {
    (void)d;
    return device_ready;
}
static uintptr_t hpm_esc_base(const struct device *d) {
    (void)d;
    return 0x10000000;
}
static void hpm_esc_read(const struct device *d, unsigned a, void *p,
                         size_t n) {
    (void)d;
    memcpy(p, process_ram + a, n);
}
static void hpm_esc_write(const struct device *d, unsigned a, const void *p,
                          size_t n) {
    (void)d;
    memcpy(process_ram + a, p, n);
}
static unsigned irq_lock(void) {
    unsigned k = interrupts_enabled;
    interrupts_enabled = false;
    return k;
}
static void irq_unlock(unsigned k) {
    interrupts_enabled = k;
}
static bool arch_irq_unlocked(unsigned k) {
    return k;
}
static bool k_is_in_isr(void) {
    return in_isr;
}
static uint64_t k_cycle_get_64(void) {
    return ++clock_cycles;
}
static uint64_t k_cyc_to_us_floor64(uint64_t c) {
    return c;
}
static int dma_config(const struct device *d, unsigned ch,
                      struct dma_config *c) {
    (void)d;
    assert(ch == 6 && c->channel_direction == MEMORY_TO_MEMORY);
    assert(c->source_data_size == 4 && c->dest_data_size == 4 &&
           c->source_burst_length == 1);
    assert(c->block_count == 1 && c->head_block->block_size % 4 == 0);
    assert(c->head_block->source_addr_adj == 0 &&
           c->head_block->dest_addr_adj == 0);
    ++configs;
    current = *c->head_block;
    callback = c->dma_callback;
    return mode == 1 ? -EINVAL : 0;
}
static int dma_start(const struct device *d, unsigned ch) {
    (void)d;
    assert(ch == 6);
    ++starts;
    if (mode == 2) {
        return -EIO;
    }
    if (mode == 3) {
        return 0;  // no completion => bounded timeout
    }
    if (mode == 4) {
        callback(dma, NULL, 6, -EIO);
        return 0;
    }
    Perform();
    callback(dma, NULL, 6, 0);
    return 0;
}
static int dma_stop(const struct device *d, unsigned ch) {
    (void)d;
    assert(ch == 6);
    ++stops;
    return 0;
}
#include "esc_dma_functions.inc"
static uint8_t *Resolve(uint32_t address) {
    if (address >= 0x10000000 && address < 0x10010000) {
        return process_ram + (address - 0x10000000);
    }
    assert(address == (uint32_t)(uintptr_t)bounce);
    return bounce;
}
static void Perform(void) {
    memcpy(Resolve(current.dest_address), Resolve(current.source_address),
           current.block_size);
    if (mode == 5) {
        Resolve(current.dest_address)[0] ^= 1;
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    uint8_t saved[512];
    memset(process_ram, 0x5a, sizeof(process_ram));
    memcpy(saved, process_ram + 0xfe00, 512);
    if (scenario == 6) {
        mode = 5;
    }
    if (scenario == 7) {
        device_ready = false;
    }
    int initialized = dust_esc_dma_init();
    if (scenario == 6 || scenario == 7) {
        assert(initialized < 0 && dust_esc_dma_error() < 0 && !ready);
        return 0;
    }
    assert(initialized == 0 && ready && dust_esc_dma_stats.selftest == 1);
    assert(!memcmp(saved, process_ram + 0xfe00, 512));
    uint8_t tx[400], rx[96];
    for (unsigned i = 0; i < 400; ++i) {
        tx[i] = (uint8_t)(i * 7 + 3);
    }
    if (scenario >= 1 && scenario <= 4) {
        mode = scenario;
        int before = configs;
        assert(dust_esc_dma_write(tx, 0x1d00, 400) < 0 &&
               dust_esc_dma_error() < 0 && stops == 1);
        assert(!dust_esc_dma_stats.writes);
        assert(dust_esc_dma_write(tx, 0x1d00, 400) < 0 &&
               configs == before + 1);
        return 0;
    }
    int before = starts;
    assert(dust_esc_dma_write(tx, 0x1d00, 400) == 0);
    assert(!memcmp(tx, process_ram + 0x1d00, 400) && starts == before + 1);
    memset(process_ram + 0x1100, 0x37, 96);
    assert(dust_esc_dma_read(rx, 0x1100, 96) == 0 &&
           !memcmp(rx, process_ram + 0x1100, 96));
    assert(dust_esc_dma_stats.writes == 1 && dust_esc_dma_stats.reads == 1);
    before = starts;
    assert(dust_esc_dma_read(rx, 0x220, 2) == 1);
    assert(dust_esc_dma_read(rx, 0x1100, 95) == 1);
    assert(dust_esc_dma_write(tx, 0x1d00, 1600) == 1);
    in_isr = true;
    assert(dust_esc_dma_read(rx, 0x1100, 96) == 1);
    in_isr = false;
    interrupts_enabled = false;
    assert(dust_esc_dma_read(rx, 0x1100, 96) == 1);
    interrupts_enabled = true;
    assert(starts == before && dust_esc_dma_stats.irq_fallbacks == 2 &&
           !dust_esc_dma_error());
    puts(
        "Actual ESC DMA staging/completion/timeout/fail-closed contracts PASS");
}
