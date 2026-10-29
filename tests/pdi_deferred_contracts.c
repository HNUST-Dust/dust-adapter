#include <assert.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include "pdi_deferred.h"
static unsigned calls, enable_calls;
static bool enabled, inject;
void irq_enable(unsigned irq) { assert(irq==42); enabled=true; ++enable_calls; }
void irq_disable(unsigned irq) { assert(irq==42); enabled=false; }
void k_sem_reset(struct k_sem* s) { s->count=0; }
void k_sem_give(struct k_sem* s) { s->count=1; }
int k_sem_take(struct k_sem* s, int timeout) {
    assert(timeout==0 || timeout==100);
    if (!s->count) return -1;
    s->count=0; return 0;
}
static void owner(void) {
    ++calls;
    assert(!enabled);
    dust_pdi_disable(); dust_pdi_enable();
    assert(!enabled); // SSC enable during service cannot expose reentry.
    if (inject) { inject=false; dust_pdi_notify(); assert(dust_pdi_pending()); assert(dust_pdi_pending()); }
}
static void keep_disabled(void) { ++calls; assert(!enabled); }
int main(void) {
    dust_pdi_reset(); dust_pdi_enable(); assert(enabled);
    dust_pdi_notify(); assert(dust_pdi_pending()); assert(dust_pdi_pending()); assert(!enabled && calls==0);
    dust_pdi_enable(); assert(!enabled); // pending protects MainLoop enable
    dust_pdi_wait(); // consumes semaphore; must NOT consume pending event
    assert(dust_pdi_service(owner)); assert(calls==1 && enabled);
    assert(!dust_pdi_service(owner) && calls==1);
    dust_pdi_notify(); assert(dust_pdi_pending()); assert(dust_pdi_pending()); inject=true;
    assert(dust_pdi_service(owner)); assert(calls==2 && !enabled);
    assert(dust_pdi_service(owner)); assert(calls==3 && enabled);
    dust_pdi_notify(); assert(dust_pdi_pending()); assert(dust_pdi_pending()); dust_pdi_disable();
    // A handler which does not request enable preserves the SSC disable.
    assert(dust_pdi_service(keep_disabled)); assert(!enabled && calls==4);
    dust_pdi_notify(); assert(dust_pdi_pending()); assert(dust_pdi_pending());
    dust_pdi_reset(); assert(!dust_pdi_service(owner)); assert(!dust_pdi_pending());
    dust_pdi_enable(); assert(enabled);
    assert(enable_calls>0);
    puts("Deferred PDI contracts PASS");
}
