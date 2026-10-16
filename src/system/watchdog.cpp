#include "watchdog.hpp"
#include "sps_boot.h"
#include <algorithm>
#include <cerrno>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <hal/nrf_wdt.h>

LOG_MODULE_REGISTER(app_watchdog, LOG_LEVEL_INF);
namespace {
int mainChannel = -1;
k_tid_t mainThread;
int64_t permittedUntil;
app_watchdog::operation *currentOperation;

[[noreturn]] void expired(int, void *name)
{
    // ISR-safe retained marker, no flash writes. MCUboot classifies the
    // subsequent software reset as a watchdog failure for lifetime rollback.
    sps_task_watchdog_mark_reset();
    LOG_ERR("Task watchdog expired: %s", static_cast<const char *>(name));
    sys_reboot(SYS_REBOOT_COLD);
}
void check_deadline(int64_t deadline, const char *name)
{
    if (k_uptime_get() >= deadline) expired(mainChannel, const_cast<char *>(name));
}
void arm_until(int64_t deadline, const char *name)
{
    const int64_t now = k_uptime_get();
    if (now >= deadline) expired(mainChannel, const_cast<char *>(name));
    const auto remaining = static_cast<uint32_t>(deadline - now);
    if (mainChannel >= 0) task_wdt_delete(mainChannel);
    permittedUntil = deadline;
    mainChannel = task_wdt_add(remaining ? remaining : 1U, expired, const_cast<char *>(name));
    if (mainChannel < 0) expired(mainChannel, const_cast<char *>("watchdog channel allocation"));
}
int init()
{
    const device *hardware = DEVICE_DT_GET(DT_NODELABEL(wdt30));
    if (!device_is_ready(hardware)) return -ENODEV;
    // Bootloader starts this same channel at the fallback's configured period.
    // Adopting it through Zephyr does not stop or restart the hardware counter.
    constexpr uint32_t hardwareMs = CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY;
    static_assert(hardwareMs == SPS_WATCHDOG_MS);
    if (nrf_wdt_started_check(NRF_WDT30) &&
        nrf_wdt_reload_value_get(NRF_WDT30) != hardwareMs * 32768U / 1000U) {
        LOG_ERR("Watchdog handoff mismatch; install the matching MCUboot image");
        return -EINVAL;
    }
    int rc = task_wdt_init(hardware);
    if (rc) return rc;
    mainThread = k_current_get();
    arm_until(k_uptime_get() + app_watchdog::normalTimeoutMs, "main");
    // task_wdt_add() in this SDK does not propagate wdt_setup() failures.
    // Check the adopted driver/channel before permitting normal startup.
    rc = wdt_feed(hardware, 0);
    if (rc) return rc;
    LOG_INF("Zephyr task watchdog: main 30 s; hardware fallback %u s", hardwareMs / 1000U);
    return 0;
}
int initialize()
{
    int rc = init();
    if (rc) {
        LOG_ERR("Watchdog initialization failed: %d; refusing unsupervised startup", rc);
        // In a normal MCUboot boot its inherited hardware watchdog is still
        // running. Do not allow startup with a silently disabled watchdog.
        k_panic();
    }
    return rc;
}
}
SYS_INIT(initialize, POST_KERNEL, 99);

void app_watchdog::progress()
{
    if (k_current_get() != mainThread) return;
    check_deadline(permittedUntil, "main");
    if (currentOperation) arm_until(currentOperation->deadline_, currentOperation->name_);
    else {
        permittedUntil = k_uptime_get() + normalTimeoutMs;
        task_wdt_feed(mainChannel);
    }
}
app_watchdog::operation::operation(uint64_t timeoutMs, const char *name)
{
    if (k_current_get() != mainThread) return;
    check_deadline(permittedUntil, "main");
    owned_ = true;
    previous_ = currentOperation;
    name_ = name;
    deadline_ = k_uptime_get() + static_cast<int64_t>(std::clamp<uint64_t>(timeoutMs, 1U, flashTimeoutMs));
    if (previous_) deadline_ = std::min(deadline_, previous_->deadline_);
    currentOperation = this;
    arm_until(deadline_, name_);
}
app_watchdog::operation::~operation()
{
    if (!owned_) return;
    check_deadline(deadline_, name_);
    currentOperation = previous_;
    arm_until(previous_ ? previous_->deadline_ : k_uptime_get() + normalTimeoutMs,
              previous_ ? previous_->name_ : "main");
}
