#pragma once
#include <stdatomic.h>
typedef atomic_int atomic_t;
#define atomic_set(p, n) atomic_exchange(p, n)
#define atomic_get(p) atomic_load(p)

#define atomic_inc(p) atomic_fetch_add(p, 1)
