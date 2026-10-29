/* SPDX-License-Identifier: Apache-2.0 */
#include "pdi_deferred.h"
#if defined(CONFIG_DUST_ECAT_SM_SYNC)
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/atomic.h>
#define PDI_IRQ DT_IRQN_BY_IDX(DT_NODELABEL(esc0), 0)
K_SEM_DEFINE(pdi_wake, 0, 1);
static atomic_t pending;
/* Owner-only state. ISR only masks the line, sets pending and wakes owner. */
static bool requested = true, servicing;
struct { uint32_t version, notifications, services, waits, sync_cycles; } dust_pdi_stats = {1, 0, 0, 0, 0};
void dust_pdi_reset(void) {
    irq_disable(PDI_IRQ);
    atomic_set(&pending, 0);
    k_sem_reset(&pdi_wake);
    requested = true;
    servicing = false;
}
void dust_pdi_notify(void) {
    irq_disable(PDI_IRQ);
    atomic_set(&pending, 1);
    ++dust_pdi_stats.notifications;
    k_sem_give(&pdi_wake);
}
void dust_pdi_enable(void) {
    requested = true;
    if (!servicing && !atomic_get(&pending)) irq_enable(PDI_IRQ);
}
void dust_pdi_disable(void) {
    requested = false;
    irq_disable(PDI_IRQ);
}
bool dust_pdi_pending(void) { return atomic_get(&pending) != 0; }
bool dust_pdi_service(void (*handler)(void)) {
    servicing = true;
    if (!atomic_set(&pending, 0)) {
        servicing = false;
        return false;
    }
    (void)k_sem_take(&pdi_wake, K_NO_WAIT);
    ++dust_pdi_stats.services;
    handler();
    servicing = false;
    if (requested && !atomic_get(&pending)) irq_enable(PDI_IRQ);
    return true;
}
void dust_pdi_cycle(void) { ++dust_pdi_stats.sync_cycles; }
void dust_pdi_wake(void) { k_sem_give(&pdi_wake); }
void dust_pdi_wait(void) {
    ++dust_pdi_stats.waits;
    (void)k_sem_take(&pdi_wake, K_USEC(100));
}
#endif
