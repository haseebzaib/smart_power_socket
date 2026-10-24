# Project signing keys and firmware releases

## Permanent two-key setup

This project now has two independent Ed25519 signing pairs under `keys/ota/`.
They replace the SDK example key used by the earlier test images. The key pairs
are permanent: rebuilding or changing VERSION does not regenerate them.

| Role | Private PEM, used to sign | Public PEM, compiled into boards |
| --- | --- | --- |
| Primary/default | `keys/ota/private/primary-ed25519.pem` | `keys/ota/public/primary-ed25519.pem` |
| Alternate/recovery signer | `keys/ota/private/recovery-ed25519.pem` | `keys/ota/public/recovery-ed25519.pem` |

On this machine the private-key folder is:

```text
/home/hsb/smart_power_socket/keys/ota/private/
```

Both public keys are included in **both MCUboot and the application**. Every new
board flashed with the new merged HEX trusts both. Each firmware file carries
one signature from the selected private key; it does not need both signatures.
The application checks against the same two keys before arming an update, and
MCUboot independently checks before execution/install.

`sysbuild.conf` selects the primary private PEM by default:

```ini
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="keys/ota/private/primary-ed25519.pem"
```

`sysbuild/ota_signing.cmake` resolves this path against the current checkout,
validates it against both public keys and passes its absolute path to signing.
No machine-specific SDK path needs to be maintained in source configuration.
Missing/untrusted private PEMs fail configuration; no replacement key is
generated and the SDK example key is not used as a fallback.

`tools/export_ota_key.py` reads the two public PEMs, not the private signer. It
generates the app trust header and MCUboot's two-entry `bootutil_keys` table.
The project module replaces the SDK's single-key provider for this profile
without modifying the SDK installation. Both public keys stay embedded even
when the selected release signer changes.

## Two machines and backups

If the client's machine builds/signs firmware, copy the **same** `keys/ota/`
folder there, including the required private PEM. Do not generate another set.
Private files were retained in the checkout as requested; if committed, access
to the source repository and its clones gives access to the signing keys. Keep
these files out of the public OTA repository. An independent backup is still
needed; the alternate private key should also be saved separately.

The client does not need any private PEM to flash an already built signed HEX or
to receive OTA. You can build/sign on this machine and send the client only the
merged HEX for initial SWD or the generated DFU pair for OTA hosting.

After a completed build, run in the SDK terminal:

```sh
python tools/ota_key_info.py build-ota
```

It prints the selected signing path/fingerprint and both trusted public-key
fingerprints. It checks the linked MCUboot ELF table and application contain
both keys, not just the configuration strings. Compare the fingerprints on
both machines; the expected values are in [keys/ota/README.md](../keys/ota/README.md).
It never prints private PEM contents.

Losing the primary private key is recoverable by selecting the already trusted
recovery private key. Losing both keys and their backups requires a new key set
and a matching SWD bootloader installation. A leaked key is a different problem:
both public keys remain trusted, so switching signers does not revoke a key.
Replacing the public trust set requires installing matching MCUboot and app.

The generation command was run once:

```sh
python tools/generate_ota_keys.py
```

It refuses to overwrite any existing key, including an incomplete set. Restore
missing files from backup; do not regenerate them on a new machine.

## Build with the normal primary signer

Use an NCS 3.2.4 SDK terminal. For the first build after this signing change:

```sh
west build -p always -d build-ota -b bl54l15_dvk/nrf54l15/cpuapp --sysbuild . -- \
  '-DCONF_FILE=prj.conf;boards/bl54l15_dvk_nrf54l15_cpuapp.conf' \
  -DDTC_OVERLAY_FILE=boards/bl54l15_dvk_nrf54l15_cpuapp.overlay
python tools/verify_ota_build.py build-ota
```

Further normal primary builds can use `west build -d build-ota`.

## Build with the recovery signer

Use the supplied sysbuild overlay, keeping a separate build directory:

```sh
west build -p always -d build-ota-recovery -b bl54l15_dvk/nrf54l15/cpuapp --sysbuild . -- \
  '-DCONF_FILE=prj.conf;boards/bl54l15_dvk_nrf54l15_cpuapp.conf' \
  -DDTC_OVERLAY_FILE=boards/bl54l15_dvk_nrf54l15_cpuapp.overlay \
  -DSB_EXTRA_CONF_FILE=sysbuild/recovery-signing.conf
python tools/verify_ota_build.py build-ota-recovery
```

Further recovery builds can use `west build -d build-ota-recovery`. This selects
`keys/ota/private/recovery-ed25519.pem`. A device provisioned with the new merged
image accepts this signature without another SWD change. Keep both public PEMs
in the checkout even if the primary private PEM is unavailable.

## First installation and the earlier SDK-key firmware

Flash **the new `build-ota/merged.hex`** through SWD once on each existing board.
It contains MCUboot and the app with both project public keys. You can also use
the recovery-signed merged HEX: both bootloaders trust the identical pair.
A public key is compiled into flash here; no separate KMU provisioning command
is needed by this profile.

The old `build-swd-1.0.0/merged.hex` and published 1.0.1 test release used the SDK
example key. They are historical artifacts, not the starting firmware for the
new key setup. Those old bootloaders do not trust the new project keys, and the
new bootloader/app deliberately reject old SDK-signed images.

The first healthy run verifies backup B's signature too. If B is intact but
signed outside this new trust set, the app reprovisions B from the newly healthy,
authenticated internal image. External settings/calibration retain their own
partition. Wait for `Health=OK/60s` and `Backup=Ready` in `getotainfo` before OTA.
A valid B signed by either project key is retained through normal health checks.

## Firmware version and publishing

Edit the root `VERSION` file. For example:

```text
VERSION_MAJOR = 1
VERSION_MINOR = 0
PATCHLEVEL = 6
VERSION_TWEAK = 0
EXTRAVERSION =
```

This produces **1.0.6+0** and command **`ota=1.0.6`**. Increase the version from
the firmware currently running; requesting that exact running version is
rejected. `VERSION_TWEAK=1` would produce `1.0.6+1` and `ota=1.0.6+1`. Keep
EXTRAVERSION empty for the numeric OTA scheme.

A normal sysbuild refreshes `<build-dir>/dfu_application/` automatically and
checks source, compiled, signed and manifest versions agree. The verification
script additionally checks actual signatures, both compiled trust anchors,
partitions and merged-image contents. Upload only these matching files together:

```text
dfu_application/manifest.json
dfu_application/smart_power_socket.signed.bin
```

Use `build-ota/dfu_application/` for primary-signed releases or
`build-ota-recovery/dfu_application/` for recovery-signed releases. Both use the
same hosted URL and manifest format. Do not upload private keys, source trees,
unsigned binaries or the merged factory HEX into the OTA directory.

After publishing the incremented version, send `ota=<version>`. After installation,
`getotainfo` reports the running `FW=<version>`; verify the result on the client's
board. Host checks do not replace the actual modem/install hardware test.
