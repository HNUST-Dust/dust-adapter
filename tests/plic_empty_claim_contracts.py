#!/usr/bin/env python3
"""Compile the real workspace PLIC handler against a bounded MMIO/ISR model."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('DUST_ZEPHYR_ROOT', root.parent / 'zephyr')) / 'drivers/interrupt_controller/intc_plic.c'
s = source.read_text()
start = s.index('static void plic_irq_handler(')
brace = s.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1
handler = s[start:end]
stat_start = s.index('struct {', s.index('/* RAM diagnostics'))
stats = s[stat_start:s.index('};', stat_start)+2]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_DUST_ECAT_DC_SYNC 1
#define __maybe_unused __attribute__((unused))
#define PLIC_DRV_HAS_COMPAT(x) EDGE_BUILD
#define PLIC_TRIG_EDGE 1
#define PLIC_TRIG_LEVEL 0
typedef uintptr_t mem_addr_t;
struct _isr_table_entry { void *arg; void (*isr)(void *); };
struct plic_stats { uint16_t *irq_count; };
struct plic_data { struct plic_stats stats; };
struct plic_config { uint32_t num_irqs; struct _isr_table_entry *isr_table; };
struct device { const struct plic_config *config; struct plic_data *data; };
static uint32_t claimed, save_irq, completions, calls, fatal, trigger;
static const struct device *save_dev;
static jmp_buf fatal_env;
static unsigned diagnostic_intervals, clock_cycles;
uint32_t k_cycle_get_32(void) { return ++clock_cycles; }
void dust_wait_irq_interval(uint32_t start,uint32_t end) { assert(end>=start); ++diagnostic_intervals; }
static mem_addr_t get_claim_complete_addr(const struct device *d) { (void)d; return 0x100; }
static uint32_t sys_read32(mem_addr_t a) { assert(a==0x100); return claimed; }
static void sys_write32(uint32_t value, mem_addr_t a) {
    assert(a==0x100 && value>0 && value<4); ++completions;
}
#if EDGE_BUILD
static uint32_t riscv_plic_irq_trig_val(const struct device *d, uint32_t irq) {
    (void)d; assert(irq>0 && irq<4); return trigger;
}
#endif
__attribute__((noreturn)) static void z_irq_spurious(void *arg) {
    (void)arg; ++fatal; longjmp(fatal_env, 1);
}
static void valid_handler(void *arg) {
    assert(arg==(void*)2 && save_irq==2);
    if (EDGE_BUILD && trigger==PLIC_TRIG_EDGE) assert(completions==calls+1);
    else assert(completions==calls);
    ++calls;
}
'''
main = r'''
int main(void) {
    uint16_t counts[4] = {0};
    struct _isr_table_entry table[4] = {{0}};
    table[1].isr=z_irq_spurious;
    table[2].isr=valid_handler; table[2].arg=(void*)2;
    struct plic_config cfg={4,table};
    struct plic_data data={{counts}};
    struct device dev={&cfg,&data};
    for (unsigned i=0; i<10000; ++i) { claimed=0; plic_irq_handler(&dev); }
    assert(!calls && !completions && !fatal && !counts[0]);
    assert(dust_plic_claim_stats.empty==10000);
    claimed=2;
    for (unsigned i=0; i<10000; ++i) {
        trigger=i&1; plic_irq_handler(&dev);
    }
    assert(calls==10000 && completions==10000 && save_dev==&dev);
    assert(dust_plic_claim_stats.valid==10000);
    claimed=0; plic_irq_handler(&dev);
    assert(save_irq==2 && completions==10000);
    uint32_t invalids[]={4,UINT32_MAX};
    for (unsigned i=0; i<2; ++i) {
        claimed=invalids[i];
        if (!setjmp(fatal_env)) { plic_irq_handler(&dev); assert(0); }
    }
    assert(fatal==2 && dust_plic_claim_stats.invalid==2);
    claimed=1; trigger=PLIC_TRIG_LEVEL;
    if (!setjmp(fatal_env)) { plic_irq_handler(&dev); assert(0); }
    assert(fatal==3 && completions==10000);
    assert(dust_plic_claim_stats.entries==20004);
    if (EDGE_BUILD) {
        trigger=PLIC_TRIG_EDGE;
        if (!setjmp(fatal_env)) { plic_irq_handler(&dev); assert(0); }
        assert(fatal==4 && completions==10001); // edge acknowledgement precedes dispatch
    }
#ifdef CONFIG_DUST_ECAT_DC_WAIT_PROFILE
    assert(diagnostic_intervals==20001);
#endif
    puts("Real PLIC empty/valid/bounds/unregistered/completion contracts PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / 'contracts.c').write_text(prefix + stats + '\n' + handler + main)
    for edge in (0, 1):
        for shell in (0, 1):
            for wait in (0, 1):
                flags = ['-DCONFIG_PLIC_SHELL=1'] if shell else []
                if wait: flags += ['-DCONFIG_DUST_ECAT_DC_WAIT_PROFILE=1']
                subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', f'-DEDGE_BUILD={edge}', *flags,
                    str(path / 'contracts.c'), '-o', str(path / 'contracts')], check=True)
                subprocess.run([str(path / 'contracts')], check=True)
