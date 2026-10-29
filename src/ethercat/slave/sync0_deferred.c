/* SPDX-License-Identifier: Apache-2.0 */
#include "sync0_deferred.h"
#include "wait_profile.h"
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include "pdi_deferred.h"
static atomic_t pending;
static uint32_t first_at;
static bool business_active;
#define STAGES 22
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
static bool measuring;
static unsigned current_phase;
static uint32_t phase_at, phase_cycles[STAGES], phase_worker[STAGES];
static volatile uint32_t worker_cycles;
static uint32_t phase_worker_at;
#endif
void dust_sync0_worker_elapsed(uint32_t elapsed) {
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    worker_cycles += elapsed;
#else
    (void)elapsed;
#endif
}
struct {
    uint32_t version, services, stages, max_work, worst_lateness, reserved;
    uint64_t total[STAGES], worker_total[STAGES];
    uint32_t maximum[STAGES], worst[STAGES], worker_worst[STAGES], worker_at_max[STAGES];
} dust_sync0_phase_stats = {3, 0, STAGES, 0, 0, 0, {0}, {0}, {0}, {0}, {0}, {0}};
struct SlowCycle { uint32_t ordinal, work, lateness, phase[STAGES], worker[STAGES]; };
struct {
    uint32_t version, services, kept, overlap_cycles, histogram[7], reserved;
    struct SlowCycle slow[4];
} dust_sync0_slow_cycles = {1, 0, 0, 0, {0}, 0, {{0}}};
unsigned dust_sync0_phase(unsigned phase) {
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    if (!measuring || phase >= STAGES) return 0;
    unsigned previous = current_phase;
    unsigned key = irq_lock();
    uint32_t now = k_cycle_get_32();
    uint32_t worker = worker_cycles;
    irq_unlock(key);
    phase_cycles[previous] += now - phase_at;
    phase_worker[previous] += worker - phase_worker_at;
    phase_worker_at = worker;
    phase_at = now;
    current_phase = phase;
    return previous;
#else
    (void)phase; return 0;
#endif
}
struct { uint32_t version, services, coalesced, fault, max_lateness, max_work; }
    dust_sync0_active_stats = {1, 0, 0, 0, 0, 0};
struct {
    uint32_t version, services;
    uint64_t total_work;
    uint32_t max_work, max_lateness, histogram[7], reserved;
} dust_sync0_budget_stats = {1, 0, 0, 0, 0, {0}, 0};
void dust_sync0_set_active(bool active) { business_active = active; }
struct { uint32_t version, notifications, services, coalesced, fault,
                  period_cycles, max_lateness, max_work; }
    dust_sync0_stats = {1, 0, 0, 0, 0, 0, 0, 0};
void dust_sync0_reset(void) {
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    dust_wait_reset();
#endif
    atomic_set(&pending, 0);
    first_at = 0;
    business_active = false;
    dust_sync0_budget_stats.services = 0;
    dust_sync0_budget_stats.total_work = 0;
    dust_sync0_budget_stats.max_work = 0;
    dust_sync0_budget_stats.max_lateness = 0;
    for (unsigned i = 0; i < 7; ++i) dust_sync0_budget_stats.histogram[i] = 0;
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    measuring = false;
    dust_sync0_slow_cycles.services = 0;
    dust_sync0_slow_cycles.kept = 0;
    dust_sync0_slow_cycles.overlap_cycles = 0;
    for (unsigned i = 0; i < 7; ++i) dust_sync0_slow_cycles.histogram[i] = 0;
    dust_sync0_phase_stats.services = 0;
    dust_sync0_phase_stats.max_work = 0;
    dust_sync0_phase_stats.worst_lateness = 0;
    for (unsigned i = 0; i < STAGES; ++i) {
        dust_sync0_phase_stats.total[i] = 0;
        dust_sync0_phase_stats.worker_total[i] = 0;
        dust_sync0_phase_stats.worker_worst[i] = 0;
        dust_sync0_phase_stats.worker_at_max[i] = 0;
        dust_sync0_phase_stats.maximum[i] = 0;
        dust_sync0_phase_stats.worst[i] = 0;
    }
#endif
    dust_sync0_active_stats.services = 0;
    dust_sync0_active_stats.coalesced = 0;
    dust_sync0_active_stats.fault = 0;
    dust_sync0_active_stats.max_lateness = 0;
    dust_sync0_active_stats.max_work = 0;
    dust_sync0_stats.fault = 0;
    dust_sync0_stats.period_cycles = 0;
}
void dust_sync0_period(uint32_t cycles) {
    dust_sync0_stats.fault = 0;
    dust_sync0_stats.period_cycles = cycles;
}
void dust_sync0_begin(void) {
    // Ignore clock-settling transients before the business session is armed.
    // SSC's own DC/SM watchdog remains enabled throughout startup.
    unsigned key = irq_lock();
    dust_sync0_stats.fault = 0;
    dust_sync0_stats.max_lateness = 0;
    dust_sync0_stats.max_work = 0;
    irq_unlock(key);
}
void dust_sync0_notify(void) {
    ++dust_sync0_stats.notifications;
    bool first=atomic_inc(&pending)==0;
    if (first) {
        first_at = k_cycle_get_32();
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
        dust_wait_notify(first_at);
#endif
    }
    else ++dust_sync0_stats.coalesced;
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    uint32_t wake_begin=k_cycle_get_32();
#endif
    dust_pdi_wake();
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    if(first) dust_wait_wake(wake_begin,k_cycle_get_32());
#endif
}
bool dust_sync0_error(void) { return dust_sync0_stats.fault != 0; }
bool dust_sync0_pending(void) { return atomic_get(&pending) != 0; }
bool dust_sync0_service(void (*handler)(void)) {
    // Atomically pair first timestamp with pending count. A new IRQ after
    // release owns a new timestamp, never overwriting this local snapshot.
    unsigned key = irq_lock();
    int count = atomic_set(&pending, 0);
    uint32_t at = first_at;
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    if(count) dust_wait_take(k_cycle_get_32(),business_active);
#endif
    irq_unlock(key);
    if (!count) return false;
    const bool active = business_active;
    key = irq_lock();
    const uint32_t start = k_cycle_get_32();
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    phase_worker_at = worker_cycles;
#endif
    irq_unlock(key);
    const uint32_t late = start - at;
    if (late > dust_sync0_stats.max_lateness) dust_sync0_stats.max_lateness = late;
    if (count > 1 && dust_sync0_stats.period_cycles) dust_sync0_stats.fault |= 1;
    if (dust_sync0_stats.period_cycles && late >= dust_sync0_stats.period_cycles)
        dust_sync0_stats.fault |= 2;
    ++dust_sync0_stats.services;
    // Invoke once, never replay a queue of stale business cycles. The handler
    // sees sticky faults before processing and publishes a protected image.
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    for (unsigned i = 0; i < STAGES; ++i) { phase_cycles[i] = 0; phase_worker[i] = 0; }
    current_phase = 0;
    phase_at = start;
    measuring = active;
#endif
    handler();
    key = irq_lock();
    uint32_t end = k_cycle_get_32();
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    uint32_t worker_end = worker_cycles;
#endif
    irq_unlock(key);
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
    if (measuring) {
        phase_cycles[current_phase] += end - phase_at;
        phase_worker[current_phase] += worker_end - phase_worker_at;
    }
    measuring = false;
#endif
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
    dust_wait_complete(start,end,active && business_active);
#endif
    uint32_t work = end - start;
    if (work > dust_sync0_stats.max_work) dust_sync0_stats.max_work = work;
    if (dust_sync0_stats.period_cycles && work >= dust_sync0_stats.period_cycles)
        dust_sync0_stats.fault |= 4;
    // Count only cycles Active at entry and exit; freeze through stop handling.
    if (active && business_active) {
        ++dust_sync0_budget_stats.services;
        dust_sync0_budget_stats.total_work += work;
        if (work > dust_sync0_budget_stats.max_work) dust_sync0_budget_stats.max_work = work;
        if (late > dust_sync0_budget_stats.max_lateness) dust_sync0_budget_stats.max_lateness = late;
        const uint32_t budget_bounds[6] = {2400, 3000, 3600, 4800, 6000, 12000};
        unsigned budget_bin = 0;
        while (budget_bin < 6 && work >= budget_bounds[budget_bin]) ++budget_bin;
        ++dust_sync0_budget_stats.histogram[budget_bin];
#if defined(CONFIG_DUST_ECAT_DC_PROFILE)
        ++dust_sync0_slow_cycles.services;
        const uint32_t boundaries[6] = {2400, 3000, 3600, 4800, 6000, 12000};
        unsigned bin = 0;
        while (bin < 6 && work >= boundaries[bin]) ++bin;
        ++dust_sync0_slow_cycles.histogram[bin];
        uint32_t worker_sum = 0;
        for (unsigned i = 0; i < STAGES; ++i) worker_sum += phase_worker[i];
        if (worker_sum) ++dust_sync0_slow_cycles.overlap_cycles;
        unsigned slot = dust_sync0_slow_cycles.kept;
        if (slot < 4) ++dust_sync0_slow_cycles.kept;
        else {
            slot = 0;
            for (unsigned i = 1; i < 4; ++i)
                if (dust_sync0_slow_cycles.slow[i].work < dust_sync0_slow_cycles.slow[slot].work)
                    slot = i;
        }
        if (dust_sync0_slow_cycles.services <= 4 ||
            work > dust_sync0_slow_cycles.slow[slot].work) {
            struct SlowCycle *record = &dust_sync0_slow_cycles.slow[slot];
            record->ordinal = dust_sync0_slow_cycles.services;
            record->work = work;
            record->lateness = late;
            for (unsigned i = 0; i < STAGES; ++i) {
                record->phase[i] = phase_cycles[i];
                record->worker[i] = phase_worker[i];
            }
        }
        ++dust_sync0_phase_stats.services;
        for (unsigned i = 0; i < STAGES; ++i) {
            dust_sync0_phase_stats.total[i] += phase_cycles[i];
            dust_sync0_phase_stats.worker_total[i] += phase_worker[i];
            if (phase_cycles[i] > dust_sync0_phase_stats.maximum[i]) {
                dust_sync0_phase_stats.maximum[i] = phase_cycles[i];
                dust_sync0_phase_stats.worker_at_max[i] = phase_worker[i];
            }
        }
        if (work > dust_sync0_phase_stats.max_work) {
            dust_sync0_phase_stats.max_work = work;
            dust_sync0_phase_stats.worst_lateness = late;
            for (unsigned i = 0; i < STAGES; ++i) {
                dust_sync0_phase_stats.worst[i] = phase_cycles[i];
                dust_sync0_phase_stats.worker_worst[i] = phase_worker[i];
            }
        }
#endif
        ++dust_sync0_active_stats.services;
        if (count > 1) dust_sync0_active_stats.coalesced += count - 1;
        dust_sync0_active_stats.fault |= dust_sync0_stats.fault;
        if (late > dust_sync0_active_stats.max_lateness)
            dust_sync0_active_stats.max_lateness = late;
        if (work > dust_sync0_active_stats.max_work)
            dust_sync0_active_stats.max_work = work;
    }
    return true;
}
#endif
