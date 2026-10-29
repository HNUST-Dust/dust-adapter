#include <assert.h>
#include <stdio.h>
#include "sync0_deferred.h"
extern struct { uint32_t version, services, coalesced, fault, max_lateness, max_work; } dust_sync0_active_stats;
extern struct {
    uint32_t version, services, stages, max_work, worst_lateness, reserved;
    uint64_t total[22], worker_total[22];
    uint32_t maximum[22], worst[22], worker_worst[22], worker_at_max[22];
} dust_sync0_phase_stats;
extern struct {
    uint32_t version, services, kept, overlap_cycles, histogram[7], reserved;
    struct { uint32_t ordinal, work, lateness, phase[22], worker[22]; } slow[4];
} dust_sync0_slow_cycles;
static uint32_t now, duration;
static void timed_owner(void) { now += duration; }
static unsigned calls, wakes;
static bool inject, expect_fault;
uint32_t k_cycle_get_32(void) { return now; }
unsigned irq_lock(void) { return 0; }
void irq_unlock(unsigned key) { assert(!key); }
void dust_pdi_wake(void) { ++wakes; }
static void owner(void) {
    ++calls;
    assert(dust_sync0_error() == expect_fault);
    now += 5;
    if (inject) { inject=false; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); }
}
static void stopping_owner(void) { now += 150; dust_sync0_set_active(false); }
static void phased_owner(void) {
    now += 2;
    unsigned previous = dust_sync0_phase(DUST_SYNC_PROCESS);
    now += 11;
    dust_sync0_worker_elapsed(4);
    assert(dust_sync0_phase(DUST_SYNC_ESC) == DUST_SYNC_PROCESS);
    now += 17;
    dust_sync0_phase(previous);
    now += 3;
}
static void slow_owner(void) { ++calls; now += 100; }
int main(void) {
    dust_sync0_reset(); dust_sync0_period(100);
    now=10; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); assert(calls==0 && wakes==1);
    now=20; assert(dust_sync0_service(owner)); assert(!dust_sync0_error());
    assert(!dust_sync0_service(owner)); assert(!dust_sync0_pending());
    now=30; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); inject=true;
    assert(dust_sync0_service(owner)); assert(dust_sync0_service(owner));
    // Backlog is a sticky fault and handler sees it before business execution.
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); expect_fault=true;
    assert(dust_sync0_service(owner)); assert(dust_sync0_error());
    assert(!dust_sync0_service(owner)); assert(!dust_sync0_pending());
    // Unsigned timer wrap is valid for bounded per-period intervals.
    dust_sync0_reset(); dust_sync0_period(100); expect_fault=false;
    now=0xfffffff0U; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); now=5;
    assert(dust_sync0_service(owner)); assert(!dust_sync0_error());
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); now += 100; expect_fault=true;
    assert(dust_sync0_service(owner)); assert(dust_sync0_error());
    dust_sync0_reset(); dust_sync0_period(100);
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); assert(dust_sync0_service(slow_owner));
    assert(dust_sync0_error());
    dust_sync0_reset(); dust_sync0_period(100); expect_fault=false;
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); assert(dust_sync0_service(owner));
    assert(dust_sync0_active_stats.services == 0);
    dust_sync0_set_active(true);
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); now += 7; assert(dust_sync0_service(owner));
    assert(dust_sync0_active_stats.services == 1);
    assert(dust_sync0_active_stats.max_lateness == 7);
    assert(dust_sync0_active_stats.max_work == 5);
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); assert(dust_sync0_service(stopping_owner));
    assert(dust_sync0_active_stats.services == 1);
    assert(dust_sync0_active_stats.max_work == 5);
    dust_sync0_reset(); dust_sync0_period(100);
    dust_sync0_set_active(true);
    now=100; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); now += 7;
    assert(dust_sync0_service(phased_owner));
    assert(dust_sync0_phase_stats.services == 1);
    assert(dust_sync0_phase_stats.max_work == 33);
    assert(dust_sync0_phase_stats.worst_lateness == 7);
    assert(dust_sync0_phase_stats.total[DUST_SYNC_SSC] == 5);
    assert(dust_sync0_phase_stats.total[DUST_SYNC_PROCESS] == 11);
    assert(dust_sync0_phase_stats.total[DUST_SYNC_ESC] == 17);
    uint32_t sum = 0;
    for (unsigned i=0; i<22; ++i) sum += dust_sync0_phase_stats.worst[i];
    assert(sum == 33);
    assert(dust_sync0_phase_stats.worker_worst[DUST_SYNC_PROCESS] == 4);
    assert(dust_sync0_phase_stats.worker_total[DUST_SYNC_PROCESS] == 4);
    assert(dust_sync0_phase_stats.worker_at_max[DUST_SYNC_PROCESS] == 4);
    dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending()); assert(dust_sync0_service(stopping_owner));
    assert(dust_sync0_phase_stats.services == 1);
    assert(dust_sync0_phase(DUST_SYNC_CAN) == 0);
    dust_sync0_reset(); dust_sync0_period(10000); dust_sync0_set_active(true);
    uint32_t durations[] = {20,80,30,70,10,90};
    for (unsigned i=0; i<6; ++i) {
        duration=durations[i]; dust_sync0_notify(); assert(dust_sync0_pending()); assert(dust_sync0_pending());
        assert(dust_sync0_service(timed_owner));
    }
    assert(dust_sync0_slow_cycles.services == 6);
    assert(dust_sync0_slow_cycles.kept == 4);
    assert(dust_sync0_slow_cycles.histogram[0] == 6);
    uint32_t sum_slow=0;
    for (unsigned i=0; i<4; ++i) {
        sum_slow += dust_sync0_slow_cycles.slow[i].work;
        uint32_t sum_phases=0;
        for (unsigned j=0; j<22; ++j) sum_phases += dust_sync0_slow_cycles.slow[i].phase[j];
        assert(sum_phases == dust_sync0_slow_cycles.slow[i].work);
    }
    assert(sum_slow == 270); // largest four:30,70,80,90
    puts("Deferred Sync0 backlog/latency/work/wrap contracts PASS");
}
