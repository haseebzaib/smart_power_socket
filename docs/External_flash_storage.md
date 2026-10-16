# BL54L15 external flash storage

For `bl54l15_dvk/nrf54l15/cpuapp` with the custom carrier overlay, settings use
the MX25R6435FZNIL0 external SPI NOR (64 Mbit = 8 MiB).
[Macronix lists this device family as 64 Mbit](https://www.macronix.com/en-us/products/NOR-Flash/Pages/Ultra-Low-Power-Flash.aspx). Calibration for all four outlets,
relay state, alert thresholds/power-factor enable and heartbeat interval are
stored through Zephyr settings/NVS. Subsequent settings saves use external
flash, with no fallback to internal storage if external flash fails.

## Partition map

Addresses below are offsets within the external flash chip.

| Partition | Offset | Size | Use |
| --- | --- | --- | --- |
| `ota_image_a` | `0x000000` | `0x100000` (1 MiB) | Reserved future firmware image A |
| `ota_image_b` | `0x100000` | `0x100000` (1 MiB) | Reserved future firmware image B |
| `ota_growth_reserve` | `0x200000` | `0x200000` (2 MiB) | Unused space reserved for future layout changes |
| `settings_storage` | `0x400000` | `0x010000` (64 KiB) | Active settings/calibration NVS |
| `data_storage` | `0x410000` | `0x3f0000` | Reserved future application data |

The settings area uses sixteen 4 KiB erase sectors. The board configuration
sets the SPI NOR page layout to 4 KiB, avoiding the driver's default 64 KiB
layout, which exceeds the settings backend's supported sector size.

Each firmware image area is 1 MiB (1,048,576 bytes). The stored OTA image,
including any headers/metadata, must fit within that limit as features such as
BLE are added. This is smaller than the previous 2 MiB per image. The freed
2 MiB stays reserved so settings and application-data addresses remain stable.
If firmware later exceeds 1 MiB, revise the OTA layout before enabling updates.

The A/B areas only reserve storage: downloading, signature verification,
MCUboot integration and activation of an updated image are future work.
The application still executes from internal RRAM. Future OTA design must
account for the internal executable image and bootloader layout; these two
external areas are not automatically bootable MCUboot slots. Settings and
future application data occupy separate areas from both firmware images.

## Configuration

Select `boards/bl54l15_dvk_nrf54l15_cpuapp.overlay` for the custom carrier.
The matching `.conf` configures external settings and NVS sectors. For NCS
configurations with an explicit `CONF_FILE`, include both `prj.conf` and
`boards/bl54l15_dvk_nrf54l15_cpuapp.conf`; otherwise Zephyr selects the
board fragment automatically. For NCS
sysbuild/Partition Manager, the board-specific
`pm_static_bl54l15_dvk_nrf54l15_cpuapp.yml` is selected by board name and
qualifiers. Remove any explicit override pointing to the old `pm_static.yml`
from this board's build configuration. Use a pristine configuration when you
next build so old partition configuration is not reused.

The overlay also declares matching fixed partitions for builds without
Partition Manager. The existing `pm_static.yml` and
`boards/nrf54L15_sps.overlay` remain unchanged for the original board.
Firmware checks that the selected settings area belongs to external flash
on the new carrier and reports an error if an old partition map is selected.

## Troubleshooting initialization error -33

`-33` is `EDOM`. The NVS settings backend returns this when its sector size
exceeds 65,535 bytes. SPI NOR defaults to a 65,536-byte page layout, so the
board configuration's 4 KiB layout must be included.

The inspected build cache selected `CONF_FILE=prj.conf` only. An explicit
`CONF_FILE` bypasses automatic board-fragment selection. Set the application
configuration files in the IDE to both files, or use this CMake argument in
**your** pristine build configuration:

```text
-DCONF_FILE="prj.conf;boards/bl54l15_dvk_nrf54l15_cpuapp.conf"
```

Keep the existing board and overlay selection. After configuring, the generated
application `zephyr/.config` should contain:

```text
CONFIG_SPI_NOR_FLASH_LAYOUT_PAGE_SIZE=4096
CONFIG_SETTINGS_NVS_SECTOR_COUNT=16
CONFIG_FLASH_JESD216_API=y
```

The new startup log reports page size, NVS sector size and configured sector
count, with an explicit error if the sector is too large. The supplied failing
log confirms the partition points to the external device, but does not establish
successful settings initialization or persistence.

## Internal storage

The former internal settings region at `0x15c000`, size `0x9000`, remains
reserved as `unused_internal_storage`; the application limit stays `0x15c000`.
The firmware does not mount, read, migrate or write that internal settings area.
Calibration and device settings are loaded and saved only through external NOR
on this carrier. Internal RRAM still contains the executable firmware.

Existing internal values are not automatically copied. A fresh external
settings partition starts with the normal defaults; reapply any calibration,
relay state, thresholds or heartbeat settings you need to retain. Previously
saved external settings continue to load normally.

## Logs

At startup, look for the storage device, offset, size and reserved areas:

```text
External flash: MX25R6435FZNIL0, JEDEC=c2 28 17, capacity=8388608 bytes (8 MiB)
Settings storage: device=<NOR device> offset=0x400000 size=0x10000
Reserved OTA image A: device=<NOR device> offset=0x0 size=0x100000
Reserved OTA image B: device=<NOR device> offset=0x100000 size=0x100000
Reserved future data: device=<NOR device> offset=0x410000 size=0x3f0000
Settings ready: loaded mask=0x0f calibration mask=0x0f
```

Loaded-mask bits are
calibration, relay, alerts and heartbeat respectively; calibration-mask bits
identify outlets with saved calibration. Missing entries retain defaults.

Successful saves are read back from the settings backend and byte-compared
before success is returned. Relay commands skip flash access if the mask matches
its last successfully loaded or verified saved value. Failed writes/readbacks
invalidate that cache so a later command retries persistence. For example:

```text
Settings persisted and verified: sps/cal (24 bytes)
```

A readback mismatch or read failure reports `Settings readback ... failed`
and returns an error. This checks the saved value rather than just assuming
that a successful write call means it can be read. To check retention, change
one setting to your desired value, look for the verified-save log, then reboot
and check the corresponding loaded value. No destructive flash self-test or
write to an OTA partition is performed.

Initialization, identity, loading, save and readback failures report errors.
Saving when storage is unavailable returns an error rather than acknowledging
successful persistence. A storage failure can leave runtime values changed;
check the SMS error/logs before assuming a setting will survive reboot.

No project build, flash operation or hardware validation was run for this change.
