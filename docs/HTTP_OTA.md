# Cellular HTTPS firmware updates

Send `ota=1.0.1` to install release `1.0.1+0`, or include the build number:
`ota=1.0.1+2`. Commands are case-insensitive. The existing SMS sender rules apply.
Only one update can run at a time. `getotainfo` shows its progress and the running
firmware version (`FW`). See [signing and release versions](OTA_signing_and_releases.md)
for the two-machine key setup and the 1.0.0 SWD to 1.0.1 OTA test sequence.

The firmware version now comes from the root `VERSION` file for both signing and
SMS reports. The old `001.00` SMS label is replaced by this same MCUboot version. Increment `VERSION` before each release.
An update to the version already running is rejected; an older signed version is
allowed deliberately for recovery/testing. Version components must fit MCUboot:
major/minor 0–255, patch 0–65535, build 0–4294967295.

## Configure the repository

The carrier profile uses the public
[SPS_OTA_UPDATE repository](https://github.com/haseebzaib/SPS_OTA_UPDATE), branch
`main`. This setting is in `boards/bl54l15_dvk_nrf54l15_cpuapp.conf`:

```ini
CONFIG_SPS_OTA_BASE_URL="https://raw.githubusercontent.com/haseebzaib/SPS_OTA_UPDATE/main/dfu_application/"
```

The earlier **1.0.1+0** test pair was signed with the SDK example key. It is a
historical release and is rejected by the new project-key trust set. Install the
new merged MCUboot/app once through SWD, then publish an incremented release
signed by either project key. See [signing and releases](OTA_signing_and_releases.md).
The hosting URL and generated manifest format stay the same.
Kconfig's generic default remains empty; another profile that
does not set the URL replies `ERR OTA URL not configured`. The GitHub
profile/repository webpage is not a file-download URL. Use the raw HTTPS
directory URL; its `manifest.json` and binary
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

A normal sysbuild now automatically refreshes `<build-dir>/dfu_application/`
after generating `dfu_application.zip`. Changing the root `VERSION` also
reconfigures the parent build, so the signed image and ZIP manifest use the new
version. The package step checks the source VERSION, compiled version, signed
header and manifest agree. A mismatch fails the build instead of silently
preparing an old release. It preserves the original manifest bytes and named
binary. Deleted or edited extracted files are restored on the next build.

Use the SDK terminal; the example build directory here is `build-ota`:

```sh
west build -d build-ota
python tools/verify_ota_build.py build-ota
```

The second command remains recommended for full partition/signature checks; it is
no longer required just to create/update the extracted folder. The final build
log includes `Release folder: ... (version ..., ... bytes)`.

If an older checkout left an old extracted folder, update to this code and do one
pristine sysbuild using the command in [FOTA setup](FOTA_Setup.md#build-and-artifacts).
For example, root VERSION major 2, minor 0, PATCHLEVEL 0, tweak 0 builds
`2.0.0+0`; inspect the manifest in the **same build directory you just built**.
Before this fix, the SDK ZIP could be current while a manually extracted folder
was still old. To extract/check an existing ZIP without rebuilding, use
`python tools/prepare_ota_folder.py build-ota`; a stale or mismatched ZIP is
rejected. Never repair a mismatch by editing manifest versions by hand.

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
   using either of the same two project public keys trusted by MCUboot. The standard SDK manifest has
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
OTA 1.0.6+0 accepted; checking manifest
OTA 1.0.6+0: Downloading firmware...
OTA 1.0.6+0: Downloading firmware: 25%
OTA 1.0.6+0: Downloading firmware: 50%
OTA 1.0.6+0: Download complete; saving firmware...
OTA 1.0.6+0: Saving firmware: 75%
OTA 1.0.6+0: Firmware saved; checking signature...
OTA 1.0.6+0 verified; rebooting to install
```

Progress notices go only to the OTA command sender. Stage changes are announced
as they happen. Percentage checkpoints show the latest value when crossing a
25% boundary, at least 30 seconds apart; a long-running phase also reports about
once per minute. Fast transfers can skip intermediate values. While no data is
arriving, the message says `Waiting for download data: ...%`; a slow manifest
fetch reports `Checking release information...`. A download awaiting its final
HTTP result never claims 100% completion.

These intermediate notices are best-effort. At most one is queued, five outbox
slots remain available after inserting it, and an unsent old stage is superseded
by the current stage. Failure or completed verification removes any remaining
optional notice before queuing the essential result. Progress cannot overwrite
the completion SMS ID used to gate reboot. Normal command replies/alerts keep
their retry behavior; delivery still depends on cellular registration and the
shared outbox. The OTA controller does not require successful progress-message delivery to continue.

On failure, one error reply names the failing step, for example:
`ERR OTA 1.0.1+0: manifest version mismatch (-116)`. No install reboot is requested
for a failed download or verification. The existing application/settings remain
in place. Partial A is discarded on the next attempt; B is retained. A reset
before the pending marker leaves the existing firmware running; once armed, a
reset may install the verified image even if the completion SMS was still pending.

`getotainfo` Update values include `Checking`, `Manifest`, `Download`, `Stage`,
`Verify`, `Pending`, `Failed`, `Idle`, and `Recovery`. A failed job returns to Idle
once its error response is queued. Logs name the accepted manifest/version/length,
HTTPS byte progress about every 30 seconds, staging progress in roughly 10%
steps, signature/readback result and reboot.

## Modem and verification limits

The implementation follows the HTTP-to-filesystem/CFS approach in the supplied
SPA reference, with strict binary framing. TLS 1.2 and host SNI are configured.
The modem needs enough free filesystem space for the signed image and manifest;
its filesystem may limit usable releases below the external slot's 1,032,192-byte
limit. Each download has a 30-minute overall application deadline, plus a
three-minute limit without increasing received-byte progress. Idle without a
final HTTP result gets a 10-second grace period and then fails; an idle modem or
a complete-looking file is never treated as HTTP success by itself. Read-only
status queries tolerate up to two consecutive failures, with five-second spacing
before retry; the third consecutive failure aborts the job.
The start-download command and partial binary reads are not blindly retried.
Location refresh is paused during a job to keep its long modem query out of the
download path.

Before opening/deleting a destination, the transport queries native modem state.
A download can survive an MCU reset because the modem remains powered. If one is
still active, the new request fails as busy, and failure cleanup cancels the old
transfer through PDP deactivation. Retry the OTA command after data reconnects.
A lost start-command OK also leaves the request tracked as possibly active, so
cleanup reconciles/cancels it. An accepted PDP-deactivation command does not by itself prove the HTTP engine
has stopped; every later request must pass the native-idle check, including
after a failed cancellation. File sessions are resynchronized after startup or a
failed close. A timed-out or malformed file-size response cannot authorize
deleting/reusing the old destination.

The HTTPS URL must use the host configured for TLS/SNI. Progress reports are
checked against the requested file-size bound; unknown total length (zero) is
allowed, but downloaded byte counts must stay within the limit. A final HTTP 200
is still required. Both manifest and binary filesystem lengths must agree with
the HTTP completion report. Progress limits are enforced when the application
polls the modem; they do not impose a native filesystem quota.

Server certificate-chain validation depends on the SIM7080 firmware's HTTPTOFS
TLS profile and provisioned certificates; this implementation does not provision
CA certificates or claim verified server identity. It does not request disabling
certificate checks. Firmware authenticity is independently enforced by the
Ed25519 signature before arming and again by MCUboot. Both project public keys
are fixed trust anchors; primary or recovery private PEM can sign subsequent
releases. Builds fail for a missing/untrusted signer instead of using the SDK
example key. See [signing and releases](OTA_signing_and_releases.md).

The host tests require a C/C++20 toolchain with sanitizers, OpenSSL development
headers/libraries, and the NCS/Zephyr source tree recorded in the build cache.

The modem commands/framing are described in the SIMCom-authored
[AT command manual](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/unit/sim7080g/en/SIM7070_SIM7080_SIM7090%20Series_AT%20Command%20Manual_V1.04.pdf)
and [HTTP(S) application note](https://files.waveshare.com/upload/b/bb/SIM7070_SIM7080_SIM7090_Series_HTTP(S)_Application_Note_V1.02.pdf).

Host checks execute the production AT parser, modem HTTPS/file methods, manifest
parser with the SDK's JSON implementation, signature verifier with real OpenSSL
crypto, and controller success/failure/SMS sequencing. They include lost replies,
completion during a failed query, duplicate URCs, an orphan native transfer,
size-limit violations, stalled downloads, missing completion and CFS recovery.
MCUboot/app builds and
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
