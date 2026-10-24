# BL54L15 MCUboot and FOTA foundation

This profile is for `bl54l15_dvk/nrf54l15/cpuapp` on the custom SPS carrier,
using the MX25R6435FZNIL0 external flash. It builds MCUboot and a signed application
with NCS 3.2.4. Cellular HTTPS updates are available through `ota=<version>`;
see [HTTP OTA](HTTP_OTA.md) for repository configuration, release files and use.
BLE transfer and fleet rollout remain future work. HTTPS transport checks include
native-modem idle reconciliation, bounded status-query retries, progress/size
limits and recovery of uncertain file sessions; see HTTP_OTA.md for deadlines.

## Execution and update flow

The CPU always executes the application from internal RRAM. External flash
cannot be executed as an alternative A/B application on this carrier.

1. Let the application complete one minute of main-loop progress with settings
   initialized. The initial factory run saves its signed image to backup B.
2. The HTTPS controller (or a future BLE transport) calls the shared
   `firmware_update::begin()` writer.
   This first archives the current healthy internal firmware into B and verifies
   readback, then erases staging A. A previous backup is not overwritten just
   because a newly installed image becomes healthy.
3. Feed the complete `zephyr.signed.bin` through sequential `write()` calls.
   Network retries must not replay an already accepted chunk. There is currently
   no interrupted-download resume protocol; call `abort()` and begin again.
4. `finish()` requires the exact expected byte count, a supported image header
   and TLV extent, CRC readback equality, and a SHA-256 match of the entire file.
   The application also verifies the Ed25519 image signature with MCUboot's
   configured public key before setting the pending marker. The writer does not
   reboot itself.
5. The HTTPS controller waits for the modem to accept its completion SMS before
   rebooting. MCUboot independently authenticates the image hash and Ed25519
   signature before installing it. The standard DFU manifest does not contain
   a download hash; both readback integrity and signature authentication apply.
6. MCUboot swaps A with the internal primary using scratch storage. Its swap
   journal supports resuming an interrupted swap. B remains untouched.
7. The application health check clears the watchdog failure streak after another
   one minute. B continues to retain the previous healthy version until the
   next download begins.

The writer API is in [firmware_update.hpp](../src/ota/firmware_update.hpp).
HTTPS now uses this shared writer; BLE can use it later. The downloader consumes
exactly the manifest and named signed binary from the SDK's DFU ZIP. For the full
flow, failure behavior and network/TLS limits, see [HTTP OTA](HTTP_OTA.md).

## Lifetime watchdog recovery

The application now uses Zephyr's task watchdog with a **30-second normal main
thread deadline** and a **20-second hardware fallback**. MCUboot starts WDT30 at
20 seconds; the application adopts the same hardware channel through the Zephyr
watchdog driver without stopping its counter. It runs during CPU sleep and pauses
when a debugger halts the CPU. Zephyr's watchdog timer handles hardware feeding.

There are no direct application watchdog feeds in UART reads, AT polling loops or
flash-copy loops. One main-loop heartbeat reports ordinary progress. The common
AT wait helper and high-level OTA operations declare finite allowances for known
long operations. A responsive but disconnected modem is not a firmware failure.
See [Watchdog.md](Watchdog.md) for deadlines, handoff details and testing.

A task watchdog timeout stores an ISR-safe marker in GPREGRET[1] and requests a
software reset. MCUboot consumes that marker and classifies it as a watchdog
failure; both task and hardware watchdog resets count toward the same three-failure
rollback policy. Ordinary software reboots do not count. GPREGRET[0] is left for
SDK boot mechanisms; GPREGRET[1] is reserved for this marker.

MCUboot captures and clears the hardware reset flags on each boot, retaining
the captured cause for application diagnostics, and records watchdog resets in
a separate external NVS journal:

- First watchdog reset: count 1; keep the current firmware.
- Second watchdog reset: count 2; keep the current firmware.
- Third consecutive watchdog reset: recover backup B through staging A and the
  normal authenticated MCUboot installation path, including after the current
  firmware was previously accepted as healthy.
- A non-watchdog reboot breaks the consecutive streak. One minute of completed
  main-loop progress with working settings also clears it. The healthy interval
  begins at the first completed loop, not merely at kernel startup.

The application must keep this watchdog/health/update contract in future images.
A firmware that still makes progress while behaving incorrectly is not diagnosed
by a watchdog; dedicated application health checks can be added later.

Standard MCUboot test updates revert at the next reboot. This project instead
uses **permanent MCUboot requests plus its own lifetime recovery journal**, so
normal reboots do not cause immediate rollback. This is not hardware security
anti-rollback: older signed firmware is deliberately allowed for recovery.

Recovery phases are persisted with CRC/readback checks. Before repurposing A,
MCUboot first completes any existing native swap. A reset during backup-to-A
copy restarts that copy from intact B. Once the recovery swap is armed, A is not
erased again; native MCUboot resumes its own swap. Signature validation remains
in MCUboot, including on the primary image at every boot. If primary validation
fails in a completed/idle state, the recovery hook also attempts verified B.
Ambiguous unfinished native swap metadata is preserved rather than overwritten.

Journal or backup corruption is logged and prevents destructive recovery writes.
Recovery requires readable external flash and a valid backup. Before the initial
one-minute factory health check, no previous image has been provisioned. The
initial B image equals the factory image; a different previous version becomes
available after the first update. Restoring an identical image cannot correct a
bug already present in that same factory version. Hardware flash failure cannot
be repaired by this software recovery mechanism.

## Zephyr watchdog supervision

The project uses Zephyr's
[task watchdog](https://docs.zephyrproject.org/latest/services/task_wdt/index.html),
not a worker that feeds the hardware unconditionally. `src/system/watchdog.cpp`
owns task channels, deadlines, hardware handoff and the timeout callback.

The single main-loop progress call supervises normal execution. A shared RAII
scope in the common AT wait helper grants the command's configured timeout plus
5 seconds. OTA erase/archive/write/verification uses a maximum 180-second scope.
Scope exit restores the normal 30-second deadline. Nested scopes cannot extend
their parent's deadline, and progress during a scope cannot renew it indefinitely.
Other threads cannot refresh or extend the main channel; future critical workers
need their own Zephyr task-watchdog channels.

The healthy interval remains **60 seconds for testing**. To use five minutes in
production, change `SPS_HEALTHY_MS` to `300000U` in
`modules/sps_boot/src/sps_boot.h` and rebuild. This delay is separate from both the
30-second task deadline and 20-second hardware fallback; after rollback the healthy
interval comes from the restored firmware binary.

## Field status: `getotainfo`

See [the command reference](all_cmds.md#getotainfo) for the response and field
meanings. The command reports internal execution, current image origin, freshly
checked backup readiness, health state, application boot count, total watchdog
resets, current failure streak, completed recovery count/latest reason and update
state. It also reports `FW`, the running image version. It does not expose AT
commands or trigger an update. See [signing and release versions](OTA_signing_and_releases.md)
for checking keys across build machines and preparing a version increment.

History has its own CRC/readback-checked NVS record (ID 2), leaving the original
boot-policy ABI (ID 1) and settings format unchanged. The bootloader commits the
recovery history before clearing its armed state, with a marker to avoid counting
the same recovery again if power fails during completion. History survives normal
reboots and healthy checks. Telemetry failure is logged independently and cannot
disable the existing rollback policy. Diagnostic counts can miss/repeat an event
if power fails between separate record commits; they are not an exact power-loss
audit. They saturate instead of wrapping.

Install the updated MCUboot as well as the application for history tracking.
A restored firmware image must contain `getotainfo` to answer that command.
An older backup containing a previous firmware without it will not gain the
command automatically. Origin `Updated` means a changed image fingerprint;
it does not distinguish an OTA install from reprogramming the application by SWD.

## Flash layout

See [External_flash_storage.md](External_flash_storage.md) for the full map.
Both external image areas and the internal primary slot are 1 MiB. Each signed
update file must fit within **0xfc000 bytes (1,032,192 bytes)**, leaving a
conservative 16 KiB for MCUboot swap trailers. That limit includes the 2 KiB
header, firmware payload and TLVs, not just the application executable.

MCUboot reserves internal `0x000000..0x020000` (128 KiB). The signed primary
starts at `0x020000`; application vectors/code start at `0x020800`. A matching
SWD installation of MCUboot plus the newly linked application is required. The
new 20-second handoff is incompatible with the earlier 30-second boot watchdog;
flash the new merged image for this change rather than updating the application
alone.
The old application linked at zero cannot acquire a bootloader through an
ordinary application-only update.

External settings remain at `0x400000`, size `0x10000`. Boot-policy NVS is separate
at `0x202000`, size `0x2000`; changing global `CONFIG_NVS_DATA_CRC` was avoided to
retain the existing settings format. Its record ABI in `sps_boot.h` must remain
compatible between MCUboot and future application releases. Settings/calibration
schema changes also need backward compatibility with the firmware retained in B.

## Bootloader self-protection disabled

The current profile explicitly selects:

```text
CONFIG_FPROTECT=n
CONFIG_NCS_MCUBOOT_DISABLE_SELF_RWX=n
```

This disables MCUboot's RRAM self-protection. It is separate from firmware-image
verification: Ed25519 signature verification and `CONFIG_BOOT_VALIDATE_SLOT0=y`
remain enabled. Invalid update signatures are still rejected before installation.
The watchdog, rollback journal and external settings behavior do not change.

The first-board error `Protect mcuboot flash failed, cancel startup` came from
reserving 128 KiB while FPROTECT was enabled. In NCS 3.2.4 the two RRAMC protection
regions can cover at most 62 KiB together, and MCUboot's 4 KiB request alignment
makes a 60 KiB partition the largest fitting size with that configuration.
[Nordic also documents disabling FPROTECT for a larger bootloader](https://devzone.nordicsemi.com/f/nordic-q-a/126139/in-ncs-version-3-2-0-if-i-change-the-mcuboot-partition-size-to-64kb-the-system-fails-to-start/557031).

Per the requested profile, the bootloader reservation is now 128 KiB again,
FPROTECT is disabled, and regular immediate RTT logging is restored. The previous
60 KiB restriction, unused gap and protection-size guards have been removed.
`tools/verify_ota_build.py` checks that self-protection is disabled, the executable
fits the reservation, and signature verification remains configured and passes
real image verification checks.

The tradeoff is that application code or an erroneous flash write can overwrite
MCUboot. A/B recovery restores only the application; it cannot repair a damaged
bootloader, which would need SWD reprogramming. This choice removes bootloader
write protection, not the image-signature checks.

Reflash **the merged MCUboot/application image** to apply the bootloader option.
Application code remains at 0x20800 and every external partition keeps its address.
No external-flash erase is needed. Empty backup B is expected before the first
healthy application run: the protection failure prevented the application from
reaching the one-minute backup provisioning step. A stored failure streak clears
on a non-watchdog reset or after the healthy check succeeds.

## Build and artifacts

Use an NCS 3.2.4 toolchain terminal:

```sh
west build -p always -d build-ota -b bl54l15_dvk/nrf54l15/cpuapp --sysbuild . -- \
  '-DCONF_FILE=prj.conf;boards/bl54l15_dvk_nrf54l15_cpuapp.conf' \
  -DDTC_OVERLAY_FILE=boards/bl54l15_dvk_nrf54l15_cpuapp.overlay
python tools/verify_ota_build.py build-ota
./tests/ota/run.sh build-ota/smart_power_socket/zephyr/zephyr.signed.bin
./tests/watchdog/run.sh
./tests/cellular/run.sh
./tests/http_ota/run.sh build-ota
```

The host recovery tests need a C compiler supporting AddressSanitizer and
UndefinedBehaviorSanitizer. The artifact checker uses SDK Python packages,
`imgtool`, PyYAML and IntelHex. It checks the actual partition map/configuration,
verifies the signed file, rejects tampered and wrong-key files, checks merged
image addresses and emits `build-ota/ota_manifest.json` with file size/SHA-256.
It also refreshes `build-ota/dfu_application/` from the current ZIP for publication;
upload its standard `manifest.json` and named `.signed.bin` together. The separate
`ota_manifest.json` is diagnostic and is not the hosted manifest.
It does not contact a server or a connected target.

- Factory/SWD image: `build-ota/merged.hex` (MCUboot plus signed application).
- Application OTA file: `build-ota/smart_power_socket/zephyr/zephyr.signed.bin`.
- MCUboot image: `build-ota/mcuboot/zephyr/zephyr.hex`.
- Image version: root `VERSION`, currently `1.0.1+0`. Increment for each release;
  SMS reports use the same generated MCUboot version, replacing `001.00`.
- Test-only SDK signing key is currently selected by sysbuild. Before deploying
  remote updates, configure `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` with your own
  protected Ed25519 key and install a matching MCUboot once. Keep the private
  key on the signing system, not in a public repository or firmware download.

No automatic flash operation is part of these commands. For the first SWD
installation, select the new merged image and do not mass-erase the external
chip if its settings must be retained. The newly assigned image/journal areas
are initialized only by their corresponding boot/update operations.

## Logs and board validation

Expected milestones include:

```text
Boot watchdog: 20 s, runs in sleep, pauses while debugger halted
Zephyr task watchdog: main 30 s; hardware fallback 20 s
Watchdog failure streak: 0/3; backup=not provisioned
MCUboot: internal primary=1 MiB; external A=staging, B=known-good, each 1 MiB
Saving known-good internal firmware to backup B (... bytes)
Known-good backup B verified and committed
Firmware healthy: watchdog streak cleared; known-good backup B retained
```

Download/recovery milestones:

```text
OTA ready: signed file ... bytes, staging A; backup B protected
OTA SHA-256, readback and Ed25519 signature verified; reboot ready
Watchdog failure streak: 3/3; backup=available
Restoring known-good backup B through MCUboot slot A
Known-good firmware restored; journal result=0
```

Builds and simulated recovery tests are not hardware qualification. On the carrier:

1. Install the merged factory image; verify JEDEC/settings logs and both boot
   stages. Wait for the healthy/backup messages. Confirm existing settings survive.
2. Check normal cellular startup, SMS waits and meter retries for false watchdog
   resets. Disable/leave the debugger running during timeout tests; a halted
   debugger pauses the watchdog.
3. Use a separate test image to intentionally stop main-thread progress without
   reporting progress. Confirm ~30-second task-watchdog resets (or a 20-second
   hardware reset if the CPU/interrupts stop), counts 1 then 2 then 3, and recovery to
   the previous version. Repeat after the updated image first passes health.
4. Use the HTTPS command with an incremented signed release. Check that a wrong
   manifest version, truncation or invalid signature is rejected before reboot,
   and a correct release installs. See HTTP_OTA.md for the full first-unit checks.
5. Interrupt power during download, B archive, recovery staging and native swap.
   Check recovery logs and settings retention after each interruption. The first
   backup/update must be provisioned before expecting rollback to another version.

There is no field-accessible intentional crash SMS command in this change.
