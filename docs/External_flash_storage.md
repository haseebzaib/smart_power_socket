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
| `mcuboot_secondary` (A) | `0x000000` | `0x100000` (1 MiB) | Signed update staging and native MCUboot swap storage |
| `ota_image_b` (B) | `0x100000` | `0x100000` (1 MiB) | Previous known-good signed image, retained for lifetime rollback |
| `mcuboot_scratch` | `0x200000` | `0x002000` (8 KiB) | Native MCUboot swap scratch |
| `nvs_storage` | `0x202000` | `0x002000` (8 KiB) | Separate boot-policy journal: watchdog streak and backup/recovery metadata |
| `ota_growth_reserve` | `0x204000` | `0x1fc000` | Unused growth reserve |
| `settings_storage` | `0x400000` | `0x010000` (64 KiB) | Existing settings/calibration NVS, address unchanged |
| `data_storage` | `0x410000` | `0x3f0000` | Reserved future application data |

The settings area uses sixteen 4 KiB sectors; the boot-policy journal uses two
separate 4 KiB sectors. Its record has its own CRC. Global NVS data CRC remains
disabled to preserve the existing settings format. SPI NOR page layout stays
at 4 KiB, avoiding the default 64 KiB layout that NVS cannot use here.

The internal primary slot is also 1 MiB. Application execution remains internal;
external A/B are storage, not directly executable slots. The signed update file
limit is 0xfc000 bytes, including header/payload/TLVs, with 16 KiB conservatively
reserved for swap metadata. MCUboot uses A to swap into the internal primary;
B retains the previous healthy version until the next update begins.

See [FOTA_Setup.md](FOTA_Setup.md) for the update-writer API, signing, watchdog
policy, build/artifact paths and hardware test procedure. HTTP and BLE transfer
and remote update commands are future work; MCUboot and local storage/recovery
infrastructure are now implemented.

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

MCUboot reserves internal `0x000000..0x020000` (128 KiB). Its self-protection is
disabled in this profile; image signature verification remains enabled. See
[FOTA setup](FOTA_Setup.md#bootloader-self-protection-disabled) for the tradeoff.
The 1 MiB primary
slot starts at `0x020000`, with a 2 KiB header and application code at `0x020800`.
The remaining internal area before the old settings reservation is unused.

The former internal settings region at `0x15c000`, size `0x9000`, remains
reserved as `unused_internal_storage`.
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
Reserved MCUboot staging A: device=<NOR device> offset=0x0 size=0x100000
Reserved boot-policy journal: device=<NOR device> offset=0x202000 size=0x2000
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

The FOTA setup is built and checked with host simulations and artifact verification.
No board flash operation or hardware validation has been performed.
