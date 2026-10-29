#pragma once
void irq_enable(unsigned);
void irq_disable(unsigned);

unsigned irq_lock(void);
void irq_unlock(unsigned);
