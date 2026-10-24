# SPS permanent OTA signing keys

Two independent Ed25519 pairs were generated once for this project.

| Role | Private signing PEM | Public trust PEM |
| --- | --- | --- |
| Normal releases | `private/primary-ed25519.pem` | `public/primary-ed25519.pem` |
| Alternate signer if the first private key is unavailable | `private/recovery-ed25519.pem` | `public/recovery-ed25519.pem` |

Both public keys are compiled into **MCUboot and the application**. A release is
signed by one private key; either is accepted. The default signer is primary.
Recovery signing uses `sysbuild/recovery-signing.conf`. Paths are resolved
relative to the project checkout so the same files work on both machines.

Public-key SHA-256 fingerprints:

```text
primary:  159d8d38a3178dabbf5e63429545fad224017042dc356ced0c3c65621a088b69
recovery: 10f3b7aa220aab4b0397fbc479673211584743d8d14222896e88a4a694c13739
```

Keep these existing keys for future releases. Do not run key generation on the
client; copy the same key folder there if it will sign builds. The generation
script refuses to overwrite any existing key. Builds never generate missing
keys or fall back to the SDK's example signing key.

Private PEMs remain in this checkout as requested. If included in version
control, the source repository and its clones contain signing secrets. The
public OTA repository receives only `dfu_application/`, never this key folder.
Keep an independent backup, preferably storing the recovery private key away
from the normal signing machine. Two keys on one lost machine alone are not a
backup. This provides an alternative when a private key is lost, not revocation
of a leaked key.

Install the new `merged.hex` through SWD once to put both public keys on each
board. An older SDK-key bootloader cannot accept these project-signed updates.
After that, build/sign here and deliver only signed images to the client; the
client does not need a private key to flash a prepared HEX or receive OTA.

Build commands and migration details: [OTA signing and releases](../../docs/OTA_signing_and_releases.md).
