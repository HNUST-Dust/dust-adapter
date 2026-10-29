#pragma once
struct k_sem { unsigned count; };
#define K_SEM_DEFINE(name, initial, limit) struct k_sem name = {initial}
#define K_NO_WAIT 0
#define K_USEC(n) (n)
void k_sem_reset(struct k_sem*);
void k_sem_give(struct k_sem*);
int k_sem_take(struct k_sem*, int);

#include <stdint.h>
uint32_t k_cycle_get_32(void);
