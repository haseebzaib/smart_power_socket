# Signing keys, two build machines and release versions

## Current test key

The current profile uses MCUboot's **SDK example Ed25519 key**. It is already
included with the SDK; this project does not create a new key on every build.
On this machine, the private PEM is:

```text
/home/hsb/ncs/v3.2.4/bootloader/mcuboot/root-ed25519.pem
```

`SB_CONFIG_BOOT_SIGNATURE_TYPE_ED25519=y` selects Ed25519. No project-specific
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` is configured, so sysbuild selects this SDK
key. It signs application releases. MCUboot embeds the corresponding public key
and uses it to check signatures. The application also embeds the public key to
verify a staged update before rebooting.

`tools/export_ota_key.py` **exports the public key only** from the configured PEM.
It does not generate a key pair, rotate keys or put the private key in firmware.
Its generated header is under the build directory:
`smart_power_socket/ota_generated/ota_public_key.hpp`. Changing firmware version
or rebuilding does not change the signing key.

The SDK example private key is publicly available, so it is suitable for this
development test, not for authenticating production releases. The current test
continues using it so both machines and the client's installed MCUboot can stay
compatible.

## Building on your machine and the client's machine

The OTA signing key must match the public key in **the bootloader actually
installed on the device**. Folder names, usernames and operating systems can
differ. The public-key fingerprint must match. In an SDK terminal, after building:

```sh
python tools/ota_key_info.py build-ota
```

For the current example key, the output includes:

```text
Ed25519 public-key SHA-256: c1907fa4eac7fae3840a78902b6f0710b037e9968e5c6274a12a28790c7d4e3c
```

Run the command on both machines and compare this fingerprint. The script reads
the actual app/MCUboot build configurations, checks both use the same public key,
and prints no private key material. `verify_ota_build.py` also checks the match.
Comparing builds verifies which key those builds use; it cannot read the key from
a device that may have been flashed with some other build.

For development, unmodified copies of the same SDK's example key should match;
confirm the fingerprint instead of relying only on the filename. Do not create
independent keys on each machine: the signatures would not match the client's
installed bootloader.

## A project-owned key for production

Generate a key **once**, retain it and use it for subsequent releases. For example,
in the SDK terminal:

```sh
mkdir -p "$HOME/sps-signing"
python "$ZEPHYR_BASE/../bootloader/mcuboot/scripts/imgtool.py" keygen \
  -t ed25519 -k "$HOME/sps-signing/sps-ed25519.pem"
chmod 600 "$HOME/sps-signing/sps-ed25519.pem"
```

Do not run key generation over an existing release key. Keep a secure backup.
Set the sysbuild string `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` to the absolute path
of that PEM on each signing machine. For example, in its sysbuild configuration:

```ini
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="/absolute/local/path/sps-ed25519.pem"
```

If both machines sign releases, securely provision **the same private PEM** on
both. Alternatively, keep the private key on one release-signing machine and
transfer only finished signed artifacts to the client. SWD flashing a prepared
HEX does not need a private key. The generated public key can be shared freely;
the private PEM must not go into the public OTA repository or DFU folder.

Switching from the SDK example key requires a matching MCUboot/application SWD
installation and a pristine build. An ordinary application-only OTA cannot
replace the public key already compiled into MCUboot. Keep the same key for
normal version updates; do not regenerate it for every release.

## Version file and OTA command

Edit the root `VERSION` file, not the generated manifest or signed binary:

```text
VERSION_MAJOR = 1
VERSION_MINOR = 0
PATCHLEVEL = 1
VERSION_TWEAK = 0
EXTRAVERSION =
```

This builds version **1.0.1+0**. For the next patch, change `PATCHLEVEL` to `2`:
version **1.0.2+0**, command **`ota=1.0.2`**. `VERSION_TWEAK` can distinguish builds
of the same patch, for example tweak `1` gives `1.0.2+1` and `ota=1.0.2+1`.
Keep EXTRAVERSION empty for this numeric OTA version scheme.

Rebuild and regenerate/check the release folder:

```sh
west build -d build-ota
python tools/verify_ota_build.py build-ota
```

The check refreshes `build-ota/dfu_application/`. Publish its `manifest.json` and
`smart_power_socket.signed.bin` **from the same build**, together in one commit,
under `dfu_application/` on the OTA repository's `main` branch. The firmware URL
stays the same as the folder's files are replaced. Do not hand-edit their version
or size. See [HTTP OTA](HTTP_OTA.md) for the initial sysbuild command and URL.

`getotainfo` now reports `FW=<running version>`, using the same generated version
as signing and `getstatus`. It reports what is executing now, not what a server
manifest advertises or which image is pending. After recovery it reports the
restored firmware's version if that backup includes this command change.

## First test: SWD 1.0.0, then OTA 1.0.1

Two builds are prepared for this test:

| Purpose | Version | Artifact |
| --- | --- | --- |
| Client SWD starting firmware, including URL and version-aware getotainfo | 1.0.0+0 | `build-swd-1.0.0/merged.hex` |
| Hosted application update | 1.0.1+0 | `build-ota/dfu_application/` |

The update pair is published in
[SPS_OTA_UPDATE](https://github.com/haseebzaib/SPS_OTA_UPDATE/tree/main/dfu_application),
commit [`eeddf04055cc`](https://github.com/haseebzaib/SPS_OTA_UPDATE/commit/eeddf04055cca3744a792f660c5d407863cbcb29).
The published signed binary is 316,196 bytes with SHA-256
`db55d7beeaaba629c21d5b7435cd532c1663fab68ea68e2fcd4ac6623ed2748f`.
Both public raw URLs were fetched successfully and matched the local artifacts.
The carrier download/install test remains to be performed by the client.

1. Flash **the 1.0.0 starting merged HEX** on the client's device using SWD.
   It includes MCUboot and the application. Do not flash the 1.0.1 merged HEX
   first if you want to demonstrate an upgrade to 1.0.1.
2. Let startup and the healthy/backup check complete. `getotainfo` should show
   `FW=1.0.0+0`, `Backup=Ready`, `Health=OK/60s` and `Update=Idle`.
3. With the 1.0.1 release folder published, send `ota=1.0.1`.
4. After verification, the completion SMS and install reboot, send `getotainfo`.
   Check **`FW=1.0.1+0`**; the image origin should be `Updated`.
5. Wait for `Health=OK/60s` again. B retains the healthy 1.0.0 starting image for
   lifetime rollback. The running program executes internally throughout.

If building the starting image on the client instead of using the prepared HEX,
set PATCHLEVEL to `0` for that build and use a separate build directory; then
restore PATCHLEVEL `1` for the hosted update. Both use the same signing key and
partition layout. Source changes made after producing these artifacts require
new builds.

A passing build/signature check does not establish cellular HTTPS connectivity.
The actual modem download, installation and field SMS checks happen on the
client's board. Do not erase external settings merely to install the SWD image.
