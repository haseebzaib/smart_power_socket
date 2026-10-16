#pragma once
#include <cstdint>
#include <cstddef>
#include <cerrno>
struct device {};
struct flash_area {};
struct NRF_WDT_Type {};
struct NRF_POWER_Type { uint32_t retained[2]; };
extern NRF_WDT_Type testHardware;
extern NRF_POWER_Type testPower;
#define NRF_WDT30 (&testHardware)
#define NRF_POWER (&testPower)
#define CONFIG_TASK_WDT_MIN_TIMEOUT 10000
#define CONFIG_TASK_WDT_HW_FALLBACK_DELAY 10000
#define RESET_SOFTWARE 2
#define RESET_WATCHDOG 1
#define DT_NODELABEL(x) 0
#define DEVICE_DT_GET(x) test_device()
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_ERR(...)
#define SYS_REBOOT_COLD 0
#define POST_KERNEL 0
#define SYS_INIT(fn,level,priority) int watchdog_test_init() { return fn(); }
using k_tid_t = void *;
using task_wdt_callback_t = void (*)(int, void *);
const device *test_device();
bool device_is_ready(const device *);
int64_t k_uptime_get();
k_tid_t k_current_get();
[[noreturn]] void k_panic();
[[noreturn]] void sys_reboot(int);
int task_wdt_init(const device *);
int task_wdt_add(uint32_t, task_wdt_callback_t, void *);
int task_wdt_delete(int);
int task_wdt_feed(int);
int wdt_feed(const device *, int);
bool nrf_wdt_started_check(const NRF_WDT_Type *);
uint32_t nrf_wdt_reload_value_get(const NRF_WDT_Type *);
inline uint32_t nrf_power_gpregret_get(const NRF_POWER_Type *p,uint32_t index) { return p->retained[index]; }
inline void nrf_power_gpregret_set(NRF_POWER_Type *p,uint32_t index,uint32_t value) { p->retained[index]=value&255; }
