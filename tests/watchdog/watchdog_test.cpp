// Test the production watchdog manager/retained reset marker. Zephyr timer and
// device APIs are mocked; the real subsystem/driver are compiled in build-ota.
#include <cassert>
#include <cstdio>
#include "system/watchdog.hpp"
#include "sps_boot.h"
#include <mock.hpp>
NRF_WDT_Type testHardware;
NRF_POWER_Type testPower{{0x42,0}};
static device hardwareDevice;
static int64_t now;
static k_tid_t owner = reinterpret_cast<void *>(1);
static k_tid_t current = owner;
static uint32_t hardwareTicks=20*32768, period;
static int64_t expiresAt;
static int feeds, adds, deletes;
static task_wdt_callback_t callback;
static void *userData;
static bool allocationFails, driverFeedFails;
struct reset {};
struct panic {};
int watchdog_test_init();
const device *test_device() { return &hardwareDevice; }
bool device_is_ready(const device *) { return true; }
int64_t k_uptime_get() { return now; }
k_tid_t k_current_get() { return current; }
[[noreturn]] void k_panic() { throw panic{}; }
[[noreturn]] void sys_reboot(int) { throw reset{}; }
int task_wdt_init(const device *d) { assert(d==&hardwareDevice); return 0; }
int task_wdt_add(uint32_t ms, task_wdt_callback_t cb, void *data) {
    ++adds; if (allocationFails) return -ENOMEM;
    period=ms; expiresAt=now+ms; callback=cb; userData=data; return 0;
}
int task_wdt_delete(int channel) { assert(channel==0); ++deletes; return 0; }
int task_wdt_feed(int channel) { assert(channel==0); ++feeds; expiresAt=now+period; return 0; }
int wdt_feed(const device *,int channel) { assert(channel==0); return driverFeedFails ? -EIO : 0; }
bool nrf_wdt_started_check(const NRF_WDT_Type *) { return true; }
uint32_t nrf_wdt_reload_value_get(const NRF_WDT_Type *) { return hardwareTicks; }
int main() {
    assert(watchdog_test_init()==0 && period==30000);
    now=1000; app_watchdog::progress(); assert(feeds==1 && expiresAt==31000);
    puts("PASS: main-loop progress feeds the Zephyr task channel");
    current=reinterpret_cast<void *>(2);
    { app_watchdog::operation ignored{180000,"other thread"}; app_watchdog::progress(); }
    assert(feeds==1 && period==30000 && expiresAt==31000);
    current=owner;
    puts("PASS: other-thread activity cannot refresh or extend main supervision");
    {
        app_watchdog::operation sms{65000,"SMS"};
        assert(period==65000 && expiresAt==66000);
        now=40000; // Longer than normal 30 s, inside the real SMS allowance.
    }
    assert(period==30000 && expiresAt==70000);
    puts("PASS: legitimate 60 s modem wait gets a bounded allowance and restores 30 s");
    {
        app_watchdog::operation flash{180000,"flash"};
        assert(expiresAt==220000);
        now=41000;
        {
            app_watchdog::operation nested{900000,"nested"};
            assert(expiresAt==220000); // Cannot extend parent or exceed cap.
            now=42000; app_watchdog::progress(); assert(expiresAt==220000);
        }
        assert(expiresAt==220000 && period==178000);
    }
    assert(period==30000 && expiresAt==72000);
    puts("PASS: nested scopes/progress cannot turn a finite allowance into an unlimited feed");
    now=72001;
    int previousFeeds=feeds;
    try { app_watchdog::progress(); assert(false); } catch (const reset &) {}
    assert(feeds==previousFeeds);
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)==(RESET_SOFTWARE|RESET_WATCHDOG));
    assert(testPower.retained[1]==0 && testPower.retained[0]==0x42);
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)==RESET_SOFTWARE);
    puts("PASS: expired task is reset and classified once, leaving SDK retention register untouched");
    sps_task_watchdog_mark_reset(); assert(sps_watchdog_classify_reset(4)==4);
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)==RESET_SOFTWARE);
    puts("PASS: stale marker cannot affect pin/power or subsequent normal software resets");
    try { callback(0,userData); assert(false); } catch (const reset &) {}
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)&RESET_WATCHDOG);
    puts("PASS: Zephyr timeout callback triggers retained watchdog classification");
    // A completed late operation must not refresh an already expired task.
    try { app_watchdog::operation late{65000,"late"}; assert(false); } catch (const reset &) {}
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)&RESET_WATCHDOG);
    puts("PASS: starting another operation cannot revive an expired channel");
    hardwareTicks=30*32768;
    try { watchdog_test_init(); assert(false); } catch (const panic &) {}
    hardwareTicks=20*32768;
    puts("PASS: incompatible bootloader watchdog period refuses unsupervised handoff");
    driverFeedFails=true;
    try { watchdog_test_init(); assert(false); } catch (const panic &) {}
    driverFeedFails=false;
    puts("PASS: failed hardware setup/feed is detected despite SDK task_wdt_add ignoring setup errors");
    allocationFails=true;
    try { watchdog_test_init(); assert(false); } catch (const reset &) {}
    assert(sps_watchdog_classify_reset(RESET_SOFTWARE)&RESET_WATCHDOG);
    puts("PASS: task-channel allocation failure cannot leave the main thread unsupervised");
    assert(adds>0 && deletes>0);
}
