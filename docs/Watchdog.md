# Zephyr task watchdog on the BL54L15

The application uses Zephyr's task watchdog (`CONFIG_TASK_WDT`) and the Zephyr
hardware watchdog driver. The normal main-thread deadline is 30 seconds. Hardware
WDT30 provides a 20-second fallback if kernel timers/interrupts stop functioning.
The one-minute firmware health interval is a separate OTA decision.

## Where supervision lives

- `src/system/watchdog.cpp`: one owner for task-watchdog initialization, hardware
  handoff, timeouts and scoped deadlines. Zephyr handles feeding the hardware.
- `src/main.cpp`: one `app_watchdog::progress()` call at the top of each loop,
  covering normal execution and the modem-not-ready retry path.
- `atEngine::wait_for_completion()`: one shared wait for both AT command overloads
  and both SMS phases. It grants the existing command timeout plus 5 seconds,
  uses elapsed uptime for the response deadline, and has no per-poll watchdog feed.
- High-level OTA operations: a finite 180-second allowance for worst-case flash
  erase/archive/write/verification. The shared low-level flash loops do not feed
  the application's watchdog.
- MCUboot uses its own hardware watchdog feeding while copying/swapping images.
  The application task-watchdog runtime does not run in the bootloader.

A scope temporarily replaces the main-thread deadline. Exiting it restores a fresh
30-second normal deadline or the remaining enclosing allowance. Nested scopes
cannot extend the enclosing deadline. Calling progress inside an active scope
cannot push its deadline forward. A late operation cannot revive an expired task.
These rules prevent an allowance from becoming an unlimited keepalive.

The present application runs its business logic on the main thread. Only that
thread can report its progress or grant an allowance. A different worker's activity
cannot conceal a stuck main thread. Future critical worker threads need separate
Zephyr task-watchdog channels. A new operation using the common AT/OTA helpers
inherits their policy; another operation expected to block longer than 30 seconds
must declare a bounded scope at its high-level entry point.

A thread that continuously feeds the hardware would mask ordinary application
hangs while the scheduler still runs. Here, the task deadline expires even when
other threads are active. If the whole CPU/kernel stops, the hardware fallback
expires instead. See the [Zephyr task watchdog documentation](https://docs.zephyrproject.org/latest/services/task_wdt/index.html).

## Bootloader handoff

MCUboot starts hardware channel 0 on WDT30 at 20 seconds. The application driver
adopts that same enabled channel, reload value, sleep behavior and debugger policy;
it does not stop the hardware counter. Initialization verifies the inherited reload
value and checks that the driver's feed succeeds, including SDK versions where
`task_wdt_add()` does not propagate a failed `wdt_setup()` result.

The board configuration enables the hardware fallback with:

```text
CONFIG_TASK_WDT=y
CONFIG_TASK_WDT_CHANNELS=2
CONFIG_TASK_WDT_HW_FALLBACK=y
CONFIG_TASK_WDT_MIN_TIMEOUT=10000
CONFIG_TASK_WDT_HW_FALLBACK_DELAY=10000
```

The SDK calculates the hardware timeout as MIN_TIMEOUT + FALLBACK_DELAY, so these
settings give 20 seconds, matching `SPS_WATCHDOG_MS` in both images. A compile-time
check keeps them consistent. The software main-thread deadline remains 30 seconds;
Zephyr's background watchdog timer services hardware during valid longer task
allowances. The hardware watchdog pauses while a debugger halts execution. Zephyr task
deadlines can still expire on resume if the system clock continued advancing
during the halt.

**Install the matching new merged MCUboot/application image for this change.**
The earlier MCUboot started hardware at 30 seconds and cannot supply the new
20-second handoff. The application reports a mismatch and refuses to run with
silently inconsistent watchdog supervision. No board was flashed automatically.

## Timeouts and recovery counting

A task timeout runs a small ISR-safe callback: store marker `0xa7` in GPREGRET[1]
and request a software reset. It performs no flash writes. MCUboot consumes and
clears the marker, accepting it only with a hardware-reported software-reset cause,
and includes `RESET_WATCHDOG` in the captured cause. The existing three-consecutive-
watchdog policy and cumulative watchdog counter therefore include task timeouts.
A stale marker cannot convert a power/pin reset or a later ordinary software reboot
into a watchdog failure. GPREGRET[0] remains available for SDK boot mechanisms;
GPREGRET[1] is reserved and must not be reused by future features.

A hardware watchdog reset needs no marker. `getotainfo` reports both types through
`WDT`, `Streak`, `Recoveries` and `Last=Watchdog`. A non-watchdog reboot or successful
healthy interval still clears the streak. History and settings storage formats
are unchanged by this refactor.

## Checks and carrier testing

```sh
./tests/watchdog/run.sh
./tests/cellular/run.sh
./tests/ota/run.sh build-ota/smart_power_socket/zephyr/zephyr.signed.bin
python tools/verify_ota_build.py build-ota
```

Host tests exercise the production deadline manager, reset-marker code, AT wait
helper/parser and recovery state machine with mocked platform/device/timer APIs.
The firmware build compiles the real Zephyr task watchdog and Nordic driver.
Host tests do not prove hardware adoption, timer delivery or register retention.

On the carrier, install the merged image and check the boot/application watchdog
logs. Test a sleeping/deadlocked main thread with other threads still running
(~30-second task timeout); a CPU/interrupt stall (~20-second hardware timeout);
and a legitimate delayed SMS submission up to 60 seconds without a false reset.
Confirm `getotainfo` counts three deliberate task timeouts and restores backup B.
Repeat with hardware watchdog faults and verify normal software reboots are not
counted. The new backup image must contain the diagnostics command to answer it
after rollback.
