#include "sps_boot.h"
#include <hal/nrf_wdt.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sps_watchdog, LOG_LEVEL_INF);
/* MCUboot only. The application adopts channel 0 through Zephyr task_wdt
 * with the same 20 s hardware period, without stopping the counter. */
void sps_watchdog_start(void)
{
    if (!nrf_wdt_started_check(NRF_WDT30)) {
        nrf_wdt_behaviour_set(NRF_WDT30, NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK);
        nrf_wdt_reload_value_set(NRF_WDT30, (SPS_WATCHDOG_MS / 1000U) * 32768U);
        nrf_wdt_reload_request_enable(NRF_WDT30, NRF_WDT_RR0);
        nrf_wdt_task_trigger(NRF_WDT30, NRF_WDT_TASK_START);
    }
    sps_watchdog_feed();
    LOG_INF("Boot watchdog: %u s, runs in sleep, pauses while debugger halted", SPS_WATCHDOG_MS / 1000U);
}
void sps_watchdog_feed(void)
{
    if (nrf_wdt_started_check(NRF_WDT30))
        nrf_wdt_reload_request_set(NRF_WDT30, NRF_WDT_RR0);
}
static int start_early(void)
{
    sps_watchdog_start();
    return 0;
}
SYS_INIT(start_early, POST_KERNEL, 99);
