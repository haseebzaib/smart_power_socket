#pragma once
#include <stdint.h>
int64_t k_uptime_get();
struct k_mutex {};
#define K_MUTEX_DEFINE(x) k_mutex x
#define K_FOREVER -1
#define K_NO_WAIT 0
inline int k_mutex_lock(k_mutex *,int) {return 0;}
inline void k_mutex_unlock(k_mutex *) {}
