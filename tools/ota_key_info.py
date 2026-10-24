#!/usr/bin/env python3
"""Show the public signing-key fingerprint for a build, without printing private key material."""
import argparse
import hashlib
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


def public_der(path):
    pem = path.read_bytes()
    try:
        key = serialization.load_pem_private_key(pem, password=None).public_key()
    except ValueError:
        key = serialization.load_pem_public_key(pem)
    if not isinstance(key, Ed25519PublicKey):
        raise ValueError(f'Expected an Ed25519 key: {path}')
    return key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


def configured_key(config, setting):
    prefix = setting + '='
    for line in config.read_text().splitlines():
        if line.startswith(prefix):
            return Path(line[len(prefix):].strip('"'))
    raise ValueError(f'{setting} missing in {config}')


def inspect_build(build):
    boot_key = configured_key(build / 'mcuboot/zephyr/.config', 'CONFIG_BOOT_SIGNATURE_KEY_FILE')
    app_key = configured_key(build / 'smart_power_socket/zephyr/.config', 'CONFIG_MCUBOOT_SIGNATURE_KEY_FILE')
    boot_public, app_public = public_der(boot_key), public_der(app_key)
    if boot_public != app_public:
        raise ValueError('Application and MCUboot use different public signing keys')
    fingerprint = hashlib.sha256(boot_public).hexdigest()
    return boot_key, app_key, fingerprint


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path, nargs='?', default=Path('build-ota'))
    args = parser.parse_args()
    boot, app, fingerprint = inspect_build(args.build_dir)
    print(f'MCUboot key file: {boot}')
    print(f'Application key file: {app}')
    print(f'Ed25519 public-key SHA-256: {fingerprint}')
    print('Application/bootloader public keys match. Compare this fingerprint on both build machines.')
