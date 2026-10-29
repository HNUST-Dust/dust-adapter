#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wait_profile.h"
struct WaitRecord { uint32_t ordinal,origin,late,irq_ack,wake_call,checkpoint,detach_cost,worker,irq,owner[10]; };
extern struct {
 uint32_t version,services,phases,max_late,max_ack,max_wake,max_checkpoint,kept;
 uint64_t total_late,total_ack,total_wake,total_checkpoint,total_worker,total_irq,owner_total[10];
 uint32_t origin_count[10],histogram[7],reserved;
 struct WaitRecord slow[4];
} dust_sync0_wait_stats;
static uint32_t now;
uint32_t k_cycle_get_32(void){return now;}
unsigned irq_lock(void){return 0;}
void irq_unlock(unsigned key){assert(!key);}
static void check_record(const struct WaitRecord *r) {
 uint32_t sum=r->detach_cost;
 for(unsigned i=0;i<10;++i) sum+=r->owner[i];
 assert(sum==r->late);
}
int main(void) {
 _Static_assert(sizeof(dust_sync0_wait_stats)==536,"RAM layout");
 dust_wait_reset();dust_wait_phase(DUST_WAIT_SLEEP);
 now=0;dust_wait_irq_enter();dust_wait_notify(5);dust_wait_wake(6,8);
 dust_wait_irq_interval(0,12);
 dust_wait_worker(1,10); // clip worker body already running before notification
 now=20;dust_wait_phase(DUST_WAIT_PDI);
 now=40;dust_wait_phase(DUST_WAIT_SYNC);
 dust_wait_take(50,true);dust_wait_complete(55,100,true);
 assert(dust_sync0_wait_stats.services==1);
 const struct WaitRecord *r=&dust_sync0_wait_stats.slow[0];
 assert(r->origin==DUST_WAIT_SLEEP && r->late==50 && r->irq_ack==5);
 assert(r->wake_call==2 && r->checkpoint==15 && r->worker==5 && r->irq==7);
 assert(r->owner[DUST_WAIT_SLEEP]==15 && r->owner[DUST_WAIT_PDI]==20);
 assert(r->owner[DUST_WAIT_SYNC]==10 && r->detach_cost==5);check_record(r);
 // A new IRQ after detach must not overwrite the detached sample.
 now=110;dust_wait_irq_enter();dust_wait_notify(115);dust_wait_take(120,true);
 now=125;dust_wait_irq_enter();dust_wait_notify(130);
 dust_wait_complete(123,140,true);
 assert(dust_sync0_wait_stats.slow[1].late==8);
 dust_wait_take(150,false);dust_wait_complete(155,160,true);
 assert(dust_sync0_wait_stats.services==2); // exclude Arm
 now=170;dust_wait_irq_enter();dust_wait_notify(171);dust_wait_take(180,true);
 dust_wait_complete(185,200,false);assert(dust_sync0_wait_stats.services==2); // Stop
 // Timer wrap and body ending before notification do not invent overlap.
 dust_wait_reset();now=0xfffffff0U;dust_wait_irq_enter();dust_wait_notify(0xfffffff5U);
 dust_wait_worker(0xffffffe0U,0xfffffff0U);
 now=5;dust_wait_phase(DUST_WAIT_PDI);dust_wait_take(10,true);dust_wait_complete(15,20,true);
 r=&dust_sync0_wait_stats.slow[0];assert(r->late==26 && r->worker==0);check_record(r);
 // Keep the four longest waits, histogram and source counts cover every accepted sample.
 dust_wait_reset();uint32_t delays[]={10,20,30,40,50,6000};
 for(unsigned i=0;i<6;++i){ now=10000+i*10000;dust_wait_irq_enter();dust_wait_notify(now);dust_wait_take(now+delays[i],true);dust_wait_complete(now+delays[i],now+delays[i]+100,true); }
 assert(dust_sync0_wait_stats.services==6 && dust_sync0_wait_stats.kept==4);
 assert(dust_sync0_wait_stats.histogram[0]==5 && dust_sync0_wait_stats.histogram[6]==1);
 uint32_t sum=0;for(unsigned i=0;i<4;++i){sum+=dust_sync0_wait_stats.slow[i].late;check_record(&dust_sync0_wait_stats.slow[i]);}
 assert(sum==6120);
 puts("Sync0 wait attribution/wake/worker clipping/concurrent notification/lifecycle/wrap/top4 contracts PASS");
}
