/* SPDX-License-Identifier: Apache-2.0 */
#include "wait_profile.h"
#if defined(CONFIG_DUST_ECAT_DC_WAIT_PROFILE)
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <string.h>
struct WaitRecord {
    uint32_t ordinal, origin, late, irq_ack, wake_call, checkpoint, detach_cost, worker, irq;
    uint32_t owner[DUST_WAIT_PHASES];
};
struct {
    uint32_t version, services, phases, max_late, max_ack, max_wake, max_checkpoint, kept;
    uint64_t total_late, total_ack, total_wake, total_checkpoint, total_worker, total_irq;
    uint64_t owner_total[DUST_WAIT_PHASES];
    uint32_t origin_count[DUST_WAIT_PHASES], histogram[7], reserved;
    struct WaitRecord slow[4];
} dust_sync0_wait_stats = {1,0,DUST_WAIT_PHASES,0,0,0,0,0,0,0,0,0,0,0,{0},{0},{0},0,{{0}}};
static struct WaitRecord current, taken;
static bool pending, checkpoint_seen, accepted;
static unsigned owner_phase;
static uint32_t irq_enter, notified, phase_at, detached, taken_notify;
void dust_wait_reset(void) {
    memset(&dust_sync0_wait_stats,0,sizeof(dust_sync0_wait_stats));
    dust_sync0_wait_stats.version=1; dust_sync0_wait_stats.phases=DUST_WAIT_PHASES;
    pending=false;accepted=false;owner_phase=DUST_WAIT_OTHER;
}
void dust_wait_irq_enter(void) { irq_enter=k_cycle_get_32(); }
// Called in Sync0 ISR only for the first queued notification.
void dust_wait_notify(uint32_t at) {
    memset(&current,0,sizeof(current));
    pending=true; checkpoint_seen=false;
    notified=phase_at=at;
    current.origin=owner_phase;
    current.irq_ack=at-irq_enter;
}
void dust_wait_wake(uint32_t begin, uint32_t end) {
    if(pending) current.wake_call=end-begin;
}
void dust_wait_phase(unsigned phase) {
    if(phase>=DUST_WAIT_PHASES) return;
    unsigned key=irq_lock();
    uint32_t now=k_cycle_get_32();
    if(pending) {
        current.owner[owner_phase]+=now-phase_at;
        phase_at=now;
        if(!checkpoint_seen) { current.checkpoint=now-notified;checkpoint_seen=true; }
    }
    owner_phase=phase;
    irq_unlock(key);
}
// Called by owner under irq_lock immediately after taking pending Sync0 work.
void dust_wait_take(uint32_t at, bool active) {
    accepted=active && pending;
    if(pending) {
        current.owner[owner_phase]+=at-phase_at;
        if(!checkpoint_seen) current.checkpoint=at-notified;
        taken=current; taken_notify=notified; detached=at;
        pending=false;
    }
}
void dust_wait_worker(uint32_t start,uint32_t end) {
    unsigned key=irq_lock();
    if(pending && (int32_t)(end-notified)>=0) {
        uint32_t age=end-notified,duration=end-start;
        current.worker+=age<duration?age:duration;
    }
    irq_unlock(key);
}
void dust_wait_irq_interval(uint32_t start,uint32_t end) {
    unsigned key=irq_lock();
    if(pending && (int32_t)(end-notified)>=0) {
        uint32_t age=end-notified,duration=end-start;
        current.irq+=age<duration?age:duration;
    }
    irq_unlock(key);
}
void dust_wait_complete(uint32_t start,uint32_t end,bool active) {
    (void)end;
    bool valid=accepted;accepted=false;
    if(!valid || !active) return;
    struct WaitRecord r=taken;
    r.ordinal=++dust_sync0_wait_stats.services;
    r.late=start-taken_notify;r.detach_cost=start-detached;
    dust_sync0_wait_stats.total_late+=r.late;
    dust_sync0_wait_stats.total_ack+=r.irq_ack;
    dust_sync0_wait_stats.total_wake+=r.wake_call;
    dust_sync0_wait_stats.total_checkpoint+=r.checkpoint;
    dust_sync0_wait_stats.total_worker+=r.worker;
    dust_sync0_wait_stats.total_irq+=r.irq;
    if(r.late>dust_sync0_wait_stats.max_late) dust_sync0_wait_stats.max_late=r.late;
    if(r.irq_ack>dust_sync0_wait_stats.max_ack) dust_sync0_wait_stats.max_ack=r.irq_ack;
    if(r.wake_call>dust_sync0_wait_stats.max_wake) dust_sync0_wait_stats.max_wake=r.wake_call;
    if(r.checkpoint>dust_sync0_wait_stats.max_checkpoint) dust_sync0_wait_stats.max_checkpoint=r.checkpoint;
    ++dust_sync0_wait_stats.origin_count[r.origin];
    for(unsigned i=0;i<DUST_WAIT_PHASES;++i) dust_sync0_wait_stats.owner_total[i]+=r.owner[i];
    const uint32_t bounds[]={240,600,1200,2400,4800,6000};
    unsigned bin=0;while(bin<6 && r.late>=bounds[bin]) ++bin;
    ++dust_sync0_wait_stats.histogram[bin];
    unsigned slot=dust_sync0_wait_stats.kept;
    if(slot<4) ++dust_sync0_wait_stats.kept;
    else { slot=0;for(unsigned i=1;i<4;++i) if(dust_sync0_wait_stats.slow[i].late<dust_sync0_wait_stats.slow[slot].late) slot=i; }
    if(r.ordinal<=4 || r.late>dust_sync0_wait_stats.slow[slot].late) dust_sync0_wait_stats.slow[slot]=r;
    accepted=false;
}
#endif
