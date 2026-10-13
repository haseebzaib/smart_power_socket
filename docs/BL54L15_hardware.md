# BL54L15 smart power socket carrier

Use `bl54l15_dvk/nrf54l15/cpuapp` (lowercase) for the Ezurio
453-00001R BL54L15 module. The DVK target supplies the module/SoC defaults;
the application overlay supplies the custom carrier wiring.

The board-qualified overlay is
`boards/bl54l15_dvk_nrf54l15_cpuapp.overlay`. Zephyr discovers it automatically.
`boards/nrf54L15_sps.overlay` retains the original carrier wiring unchanged.
Select the new overlay explicitly in the IDE for the BL54L15 carrier.

## Pin map

Pin numbers below are BL54L15 module pads, not nRF54L15 QFN pins.

| Signal | GPIO/function | Module pad |
| --- | --- | --- |
| GSM PWRKEY | P1.10 | 28 |
| Future debug TX (unused) | P1.09 | 29 |
| Cellular LED | P2.09 | 2 |
| Relay 1 | P1.08 | 30 |
| Relay 2 | P0.04 | 31 |
| Relay 3 | P1.14 | 32 |
| Relay 4 | P1.13 | 33 |
| Utility LED | P2.08 | 3 |
| FUSE_DETECTION | P1.12 (active-low input) | 34 |
| Battery voltage | P1.11 / AIN4 | 35 |
| Energy mux A | P0.03 | 36 |
| Energy mux B | P2.10 | 37 |
| MCU TX to GSM RXD | P1.04 / UART20 TX | 23 |
| MCU RX from GSM TXD | P1.05 / UART20 RX | 22 |
| GSM RI | P1.06 | 21 |
| GSM DTR | P1.07 | 20 |
| MCU energy TXD | P0.00 / UART30 TX | 17 |
| MCU energy RXD | P0.01 / UART30 RX | 18 |
| FLASH CS | P2.05 | 12 |
| FLASH CLK | P2.01 | 10 |
| FLASH IO0 / SI | P2.02 | 8 |
| FLASH IO1 / SO | P2.04 | 11 |
| FLASH IO2 / WP# | P2.03 | 13 |
| FLASH IO3 / RESET# | P2.00 | 9 |
| SWDIO | Dedicated SWDIO | 5 |
| SWDCLK | Dedicated SWDCLK | 6 |
| SWO | P2.07 | 4 |
| RESET | Dedicated nRESET | 7 |
| Y4 XL1 / XL2 | P1.00 / P1.01 | 25 / 24 |

UART20 stays at 115200 baud, 8N1, with no hardware flow control.
UART30 stays at 9600 baud, 8E1. The replacement pinctrl groups omit the
DVK RTS/CTS pins so they cannot claim GSM RI/DTR or mux A.
ADC channel 4 still selects AIN4 (P1.11), with 12-bit resolution and gain 1/4.

## Fuse detection

The user confirmed FUSE_DETECTION on P1.12, utility LED on P2.08, and
cellular LED on P2.09. This updates the LED assignments from the earlier MCU
image. The original `nrf54L15_sps.overlay` remains untouched.

FUSE_DETECTION is a GPIO input with active-low polarity: the LTV-814 pulls
it low when AC is present; R78 (47 kohm) pulls it to 3.3 V when AC is absent.
C19 (100 nF) supplies the shown 4.7 ms RC filter. No internal pull is enabled.
Firmware looks for a low pulse over up to 40 ms, sampling about every 1 ms,
so a sample near an AC zero crossing is not mistaken for an outage.

GPIO interrupts additionally time active edges continuously, including during
modem queries. Up to eight valid pulse intervals are averaged; at least three
recent intervals are needed. The LTV-814 has antiparallel input LEDs, so both
half-cycles conduct: pulse frequency is approximately twice mains frequency.
The logged mains estimate divides the measured pulse rate by two. Intervals
outside 6-15 ms reset the averaging window instead of counting glitches or
missing cycles as valid frequency samples. Frequency is diagnostic only and
does not control the AC-presence decision or fuse alert.

At each main-loop input check, logs appear on presence/frequency-validity
changes or approximately every 10 seconds (long modem operations can delay
logging):

```text
fuse_detection: AC available, freq ~60.00 Hz (pulse 120.00 Hz)
fuse_detection: AC available, freq unavailable (no active edges observed)
fuse_detection: AC unavailable, freq unavailable
```

When frequency is unavailable, a second line reports physical `raw` pin level,
cumulative active `edges`, consecutive `valid_intervals` (up to eight), cumulative
`rejected` intervals, `last_period_us`, and `edge_age_ms` (-1 before any edge).
No edges means no active transition was captured; rejected intervals mean the
observed spacing failed the 6-15 ms test. This distinguishes a steady input from
intermittent pulses without calling either an inaccurate AC measurement.

The RC filter may suppress zero-crossing edges and leave a steady low input.
That still reports AC available, with frequency unavailable; the firmware does
not invent a 50/60 Hz reading. Timing is an estimate from digital edges,
not a precision mains measurement. A pulse-timing initialization failure is
logged while the original AC-presence sampling continues.

The diagnosis assumes AC_IN is upstream of the fuse and metering is downstream.
If all metering channels are below the existing 50 V detection threshold while
input AC is detected for three consecutive network-ready alert polls, send
`Alert=Utility power lost - fuse probably blown` with the existing utility-loss
status fields. Actual delay depends on the main loop and modem operations.
This is a suspected fault: metering failures or another downstream interruption
can produce the same symptoms. With no detected input AC, the normal utility
loss alert remains. When metered socket voltage returns, the existing utility
restored alert and outlet report remain. Failed SMS sends are retried; a
successful fuse alert is not repeated until metered power is restored.

The GPIO is optional in firmware so the original overlay still works; without
its alias, only the original utility-loss behavior is available. Input init/read
errors are logged and do not establish input AC presence.

See `all_cmds.md`, `sms_commands_client.html`, and the **Fuse Detection Alert**
sheet in `SPS SMS Commands.xlsx` for the SMS behavior.

## Flash wiring

The confirmed pin list is the source for the overlay and table above.
FLASH_CLK P2.01, FLASH_IO0/MOSI P2.02, and FLASH_IO1/MISO P2.04 match
hardware SPIM00. The overlay enables SPIM00 with explicit default/sleep
pinctrl and FLASH_CS on P2.05. The fitted part is MX25R6435FZNIL0. The inherited MX25R6435F node provides the
64 Mbit capacity, JEDEC ID, SFDP, and deep-power-down parameters; its SPI
frequency is 8 MHz.

This uses single-lane hardware SPI. FLASH_IO2/WP# on P2.03 is held high
by a GPIO hog before flash initialization. FLASH_IO3/RESET# on P2.00 is
managed by the SPI NOR driver. Quad SPI is not enabled. The external device
is available as `DT_ALIAS(spi_flash)`.

P1.09 is reserved for future debug TX and is not configured by firmware.
SWDIO/SWDCLK use their dedicated pins; SWO is P2.07. Logging uses RTT,
so firmware does not assign P1.09 or drive SWO for serial logging.

Settings/NVS use the external NOR data partition at `0x400000` (64 KiB).
The board-specific `pm_static_bl54l15_dvk_nrf54l15_cpuapp.yml` reserves two
1 MiB OTA image areas, a 2 MiB growth reserve, and the remaining flash for future data. Existing
internal settings area remains reserved and unused; firmware does not mount
or migrate it. Fresh external storage uses defaults until settings are reapplied.
See [external flash storage](External_flash_storage.md).
The firmware uses external flash for calibration, relay state, alert settings
and heartbeat settings. The additional data partition remains reserved.

## Firmware review and assumptions

- Relay GPIO flags remain `GPIO_ACTIVE_HIGH`, matching the original overlay.
  At the user's request, firmware relay changes were reverted: startup restoration,
  SMS commands, and status reporting retain the original 0 = ON, 1 = OFF
  convention. This convention differs from the confirmed active-high hardware.
- Relays initialize low. Existing startup policy restores the saved mask,
  or uses `0x0f` when no saved state exists.
- Inherited DVK LEDs, buttons, PWM LEDs, and PWM20 are disabled. PWM20's
  default P1.10 output would conflict with GSM PWRKEY.
- RI has a devicetree alias but the application does not currently use it.
- LED active-low polarity, GSM PWRKEY/DTR polarity, and the battery divider
  values (110 kohm / 110 kohm) are retained from the existing project.
  Their driver/divider circuits are not visible on the supplied MCU sheet.
- Y4 is assumed to be a 32.768 kHz crystal. The inherited LFXO setting uses
  internal capacitors with `load-capacitance-femtofarad = <15500>`.
  Its actual frequency and required capacitance must be checked against Y4's
  part specification. The module supplies the high-frequency crystal.
- Existing startup code ignores relay/LED GPIO initialization errors and the
  energy-meter initialization return value. Those are remaining diagnostics
  limitations; pin migration does not address them.

## Build

Run in an nRF Connect SDK 3.2.4 toolchain terminal:

```sh
west build -p always -d build_bl54l15 -b bl54l15_dvk/nrf54l15/cpuapp --sysbuild .
```

For an IDE configuration that specifies an overlay, select the new overlay
above. The old overlay describes the original carrier and must not be selected
for the new BL54L15 wiring.
A pristine build is recommended when switching board targets.

## References

- [Zephyr BL54L15 DVK board](https://docs.zephyrproject.org/latest/boards/ezurio/bl54l15_dvk/doc/bl54l15_dvk.html)
- [Ezurio module datasheet and pad assignments](https://www.ezurio.com/documentation/datasheet-bl54l10-and-bl54l15-series)
- [Nordic dedicated pin restrictions](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/chapters/pin.html-dedicated_pins?contentId=ckWYM65jMv4DBKTI__JinQ)
- [Nordic clock pins](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/gpio.html-clock_pins?contentId=FYoairZVi5uh54YDyi_R9g)
