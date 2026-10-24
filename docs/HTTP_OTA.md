# Cellular HTTPS firmware updates

Send `ota=1.0.1` to install release `1.0.1+0`, or include the build number:
`ota=1.0.1+2`. Commands are case-insensitive. The existing SMS sender rules apply.
Only one update can run at a time. `getotainfo` shows its progress.

The firmware version now comes from the root `VERSION` file for both signing and
SMS reports. The current source version is `1.0.0+0`; the old `001.00` SMS label is
replaced by this same MCUboot version. Increment `VERSION` before each release.
An update to the version already running is rejected; an older signed version is
allowed deliberately for recovery/testing. Version components must fit MCUboot:
major/minor 0–255, patch 0–65535, build 0–4294967295.

## Configure the repository

Create a public repository under `haseebzaib`. Add this setting to
`boards/bl54l15_dvk_nrf54l15_cpuapp.conf` with your actual repository and branch:

```ini
CONFIG_SPS_OTA_BASE_URL="https://raw.githubusercontent.com/haseebzaib/REPO/main/dfu_application/"
```

`REPO` is a placeholder. The default is empty, so `ota=...` currently replies
`ERR OTA URL not configured`. The GitHub profile/repository webpage is not a
file-download URL. Use the raw HTTPS directory URL; its `manifest.json` and binary
must return HTTP 200 directly. Redirects and private repositories are unsupported.
The configured host/path must use letters, numbers, dots, hyphens, underscores
and slashes; URL credentials, query strings and custom ports are unsupported.
Use simple repository/branch names. The complete binary URL must fit 255 bytes.

Build and install this application with the actual base URL before testing remote
updates. The matching MCUboot/partition/signing setup must already be installed;
an application linked at address zero cannot add MCUboot through this command.
See [FOTA setup](FOTA_Setup.md) for the build command and initial SWD image.

## Prepare and publish a release

For example, change `VERSION` for the next release:

```text
VERSION_MAJOR = 1
VERSION_MINOR = 0
PATCHLEVEL = 1
VERSION_TWEAK = 0
EXTRAVERSION =
```

Build with sysbuild, then run in the SDK terminal:

```sh
python tools/verify_ota_build.py build-ota
```

This verifies the build and refreshes `build-ota/dfu_application/` from the latest
`dfu_application.zip`. It checks that the ZIP payload equals this build's
`zephyr.signed.bin`, and that the manifest size/version/target match. It preserves
the original manifest bytes and original named binary. Extraction alone is also
available with `python tools/prepare_ota_folder.py build-ota`.

Upload these two files together, replacing the repository's release folder:

```text
dfu_application/
  manifest.json
  smart_power_socket.signed.bin
```

The named `.signed.bin` inside the ZIP is byte-for-byte the same OTA image as
`build-ota/smart_power_socket/zephyr/zephyr.signed.bin`. Do not upload `merged.hex`,
the unsigned binary, or the ZIP itself as the download payload. The separate
`build-ota/ota_manifest.json` is a diagnostic artifact, not the hosted manifest.
Keep the published pair unchanged while devices download it. If the files change
mid-download and no longer match, devices reject the update; retry after publishing
is complete. There is no server-side release catalogue or interrupted-download
resume yet.

## What the device does

1. Require the current image's health check to have completed (`Health=OK/60s`).
   Queue an acceptance SMS and wait for the modem to accept that specific reply.
2. Use SIM7080 HTTPS to download `manifest.json` into the modem's filesystem.
   Check format version, package name, one application entry, board, SoC, linked
   address, slot indices, safe `.bin` filename, requested version and size limit.
3. Download the exact binary named by the manifest into the modem's filesystem.
   Require HTTP 200, matching download/file lengths and a matching MCUboot
   header/version. A failed check leaves staging A untouched.
4. Archive the running healthy firmware to backup B and verify it before erasing
   staging A. Copy the modem file into A in bounded binary chunks while normal
   meter/SMS processing continues. Firmware bytes bypass text/prompt parsing.
5. Require exact length, flash readback CRC/SHA-256, and the Ed25519 image signature
   using the same configured public key as MCUboot. The standard SDK manifest has
   no SHA-256 field: the controller hashes the received bytes and compares flash
   readback; the signature establishes authenticity. An optional `sha256` field
   with 64 hexadecimal characters is also checked if supplied.
6. Mark the verified image pending. Queue the completion SMS and wait for the
   modem to accept that specific message, then reboot. If submission fails, the
   generic outbox retries; `Update=Pending` remains until submission succeeds or
   the device is otherwise reset. Modem acceptance does not prove handset delivery.
7. MCUboot independently authenticates and installs A into internal RRAM. The
   application always runs internally. B retains the previous healthy firmware
   for the existing lifetime recovery after three consecutive watchdog failures.

A is always download/install staging; B is the known-good recovery copy. These
roles do not alternate. Before the next update, the current healthy application
replaces B if necessary. Do not overwrite B with an untested download.

Example replies, each individually short:

```text
OTA 1.0.1+0 accepted; checking manifest
OTA 1.0.1+0 verified; rebooting to install
```

On failure, one error reply names the failing step, for example:
`ERR OTA 1.0.1+0: manifest version mismatch (-116)`. No install reboot is requested
for a failed download or verification. The existing application/settings remain
in place. Partial A is discarded on the next attempt; B is retained. A reset
before the pending marker leaves the existing firmware running; once armed, a
reset may install the verified image even if the completion SMS was still pending.

`getotainfo` Update values include `Checking`, `Manifest`, `Download`, `Stage`,
`Verify`, `Pending`, `Failed`, `Idle`, and `Recovery`. A failed job returns to Idle
once its error response is queued. Logs name the accepted manifest/version/length,
staging progress in roughly 10% steps, signature/readback result and reboot.

## Modem and verification limits

The implementation follows the HTTP-to-filesystem/CFS approach in the supplied
SPA reference, with strict binary framing. TLS 1.2 and host SNI are configured.
The modem needs enough free filesystem space for the signed image and manifest;
its filesystem may limit usable releases below the external slot's 1,032,192-byte
limit. Each download has a 30-minute application deadline. Location refresh is
paused during a job to keep its long modem query out of the download path.

Server certificate-chain validation depends on the SIM7080 firmware's HTTPTOFS
TLS profile and provisioned certificates; this implementation does not provision
CA certificates or claim verified server identity. It does not request disabling
certificate checks. Firmware authenticity is independently enforced by the
Ed25519 signature before arming and again by MCUboot. Continue to use the same
signing key across application/bootloader releases; the SDK default key currently
selected in sysbuild is for testing. See the signing notes in FOTA_Setup.md.

The modem commands/framing are described in the SIMCom-authored
[AT command manual](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/unit/sim7080g/en/SIM7070_SIM7080_SIM7090%20Series_AT%20Command%20Manual_V1.04.pdf)
and [HTTP(S) application note](https://files.waveshare.com/upload/b/bb/SIM7070_SIM7080_SIM7090_Series_HTTP(S)_Application_Note_V1.02.pdf).

Host checks execute the production AT parser, modem HTTPS/file methods, manifest
parser with the SDK's JSON implementation, signature verifier with real OpenSSL
crypto, and controller success/failure/SMS sequencing. MCUboot/app builds and
signed-artifact checks are separate. Run:

```sh
./tests/cellular/run.sh
./tests/http_ota/run.sh build-ota
```

The HTTPS transfer has not yet been exercised on the carrier or against your
repository. First test a small version increment on one unit: check registration,
HTTP completion, file lengths, staging/signature logs, completion SMS, reboot,
new `getstatus` version and `getotainfo`. Also test a mismatched manifest/version,
missing file and interrupted transfer. BLE transport remains future work.
