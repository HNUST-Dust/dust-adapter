#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "sync0_deferred.h"
extern struct {
    uint32_t version, services; uint64_t total_work;
    uint32_t max_work, max_lateness, histogram[7], reserved;
} dust_sync0_budget_stats;
static uint32_t now, duration;
uint32_t k_cycle_get_32(void) { return now; }
unsigned irq_lock(void) { return 0; }
void irq_unlock(unsigned key) { assert(!key); }
void dust_pdi_wake(void) {}
static void owner(void) { now+=duration; }
static void stop(void) { now+=100000; dust_sync0_set_active(false); }
int main(void) {
    _Static_assert(sizeof(dust_sync0_budget_stats)==56,"budget RAM layout");
    dust_sync0_reset(); dust_sync0_period(12000);
    duration=100; dust_sync0_notify(); dust_sync0_service(owner);
    assert(!dust_sync0_budget_stats.services);
    dust_sync0_set_active(true);
    uint32_t durations[]={2000,2400,3000,3600,4800,6000,12000};
    uint64_t sum=0;
    for(unsigned i=0;i<7;++i) {
        duration=durations[i];sum+=duration;
        dust_sync0_notify(); now+=7; assert(dust_sync0_service(owner));
        assert(dust_sync0_budget_stats.histogram[i]==1);
    }
    assert(dust_sync0_budget_stats.services==7);
    assert(dust_sync0_budget_stats.total_work==sum);
    assert(dust_sync0_budget_stats.max_work==12000);
    assert(dust_sync0_budget_stats.max_lateness==7);
    assert(dust_sync0_error());
    dust_sync0_notify();dust_sync0_service(stop);
    assert(dust_sync0_budget_stats.services==7);
    dust_sync0_reset();assert(!dust_sync0_budget_stats.services);
    assert(!dust_sync0_budget_stats.total_work);
    puts("Sync0 budget histogram/lifecycle/deadline contracts PASS");
}
