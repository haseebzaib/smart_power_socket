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
| Cellular LED | P1.09 | 29 |
| Relay 1 | P1.08 | 30 |
| Relay 2 | P0.04 | 31 |
| Relay 3 | P1.14 | 32 |
| Relay 4 | P1.13 | 33 |
| Utility LED | P1.12 | 34 |
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
| FLASH CLK | P2.04 | 11 |
| FLASH IO0 / SI | P2.01 | 10 |
| FLASH IO1 / SO | P2.00 | 9 |
| FLASH IO2 / WP# | P2.02 | 8 |
| FLASH IO3 / RESET# | P2.03 | 13 |
| SWDIO | Dedicated SWDIO | 5 |
| SWDCLK | Dedicated SWDCLK | 6 |
| SWO | P2.07 | 4 |
| RESET | Dedicated nRESET | 7 |
| Y4 XL1 / XL2 | P1.00 / P1.01 | 25 / 24 |

UART20 stays at 115200 baud, 8N1, with no hardware flow control.
UART30 stays at 9600 baud, 8E1. The replacement pinctrl groups omit the
DVK RTS/CTS pins so they cannot claim GSM RI/DTR or mux A.
ADC channel 4 still selects AIN4 (P1.11), with 12-bit resolution and gain 1/4.

## Flash wiring constraint

The schematic assigns FLASH_CLK to P2.04. Hardware SPIM00 has dedicated
signals: SCK on P2.01, MOSI on P2.02, and MISO on P2.04.
P2.04 cannot be used as its hardware clock pin. The overlay therefore disables
the inherited SPIM00 flash and adds `zephyr,spi-bitbang` using the wiring above.
This provides single-lane SPI at a requested 250 kHz; actual throughput is
lower because GPIO operations add overhead. It does not enable quad SPI.
FLASH_IO2/WP# is held high by a GPIO hog before flash initialization;
FLASH_IO3/RESET# is managed by the SPI NOR driver. The external device is
available as `DT_ALIAS(spi_flash)`.

For hardware SPIM00 on a PCB revision, wire FLASH_CLK to P2.01,
FLASH_IO0 to P2.02, FLASH_IO1 to P2.04, FLASH_IO2 to P2.03, and
FLASH_IO3 to P2.00; FLASH_CS can stay on P2.05. Then replace the software
SPI node with SPIM00 pinctrl and move the flash's reset/WP assignments.
A successful firmware build does not validate electrical operation.

Settings/NVS remain in internal RRAM through `pm_static.yml`.
The firmware does not currently read or write application data to external
flash; its driver is enabled and initializes the device.

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
