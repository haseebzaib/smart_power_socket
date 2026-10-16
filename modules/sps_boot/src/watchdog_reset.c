#include "sps_boot.h"
#include <hal/nrf_power.h>
#include <zephyr/drivers/hwinfo.h>
/* Reserve GPREGRET[1] (gpregret2) for the software task-watchdog reset marker.
 * GPREGRET[0] remains available for the SDK's boot/recovery mechanisms. */
#define TASK_WDT_RESET_MARKER 0xa7U
void sps_task_watchdog_mark_reset(void)
{
    nrf_power_gpregret_set(NRF_POWER, 1, TASK_WDT_RESET_MARKER);
}
uint32_t sps_watchdog_classify_reset(uint32_t cause)
{
    uint32_t marker = nrf_power_gpregret_get(NRF_POWER, 1);
    nrf_power_gpregret_set(NRF_POWER, 1, 0);
    if (marker == TASK_WDT_RESET_MARKER && (cause & RESET_SOFTWARE))
        cause |= RESET_WATCHDOG;
    return cause;
}
