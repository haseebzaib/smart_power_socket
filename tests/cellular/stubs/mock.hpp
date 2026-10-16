#pragma once
#include <cstdint>
#include <cstddef>
struct device {};
struct ring_buf {};
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_DBG(...)
#define LOG_ERR(...)
int64_t k_uptime_get();
void k_msleep(int32_t);
int uart_fifo_read(const device *, uint8_t *, int);
inline unsigned irq_lock() { return 0; }
inline void irq_unlock(unsigned) {}
