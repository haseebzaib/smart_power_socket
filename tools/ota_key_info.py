#!/usr/bin/env python3
"""Check the selected signer against both trusted project keys; print public fingerprints only."""
import argparse
import hashlib
import json
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


def check_signer(path, trusted_paths):
    # A signer must actually contain a private key, not just a public PEM.
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    selected = public_der(path)
    trusted = [public_der(public) for public in trusted_paths]
    if len(trusted) != 2 or trusted[0] == trusted[1] or selected not in trusted:
        raise ValueError('Signing key must match one of the two distinct project public keys')
    return hashlib.sha256(selected).hexdigest()


def configured_key(config, setting):
    prefix = setting + '='
    for line in config.read_text().splitlines():
        if line.startswith(prefix):
            return Path(line[len(prefix):].strip('"'))
    raise ValueError(f'{setting} missing in {config}')


def trusted_build_keys(build):
    app = json.loads((build / 'smart_power_socket/ota_generated/trusted_keys.json').read_text())
    boot = json.loads((build / 'mcuboot/sps_keys/trusted_keys.json').read_text())
    if app != boot or app.get('schema') != 1 or len(app.get('keys', [])) != 2:
        raise ValueError('Application and MCUboot must trust the same two project keys')
    ders = [bytes.fromhex(key['der']) for key in app['keys']]
    if ders[0] == ders[1] or any(hashlib.sha256(der).hexdigest() != key['sha256']
                              for der, key in zip(ders, app['keys'])):
        raise ValueError('Invalid/duplicate public key in generated trust metadata')
    return app['keys']


def inspect_build(build):
    boot_key = configured_key(build / 'mcuboot/zephyr/.config', 'CONFIG_BOOT_SIGNATURE_KEY_FILE')
    app_key = configured_key(build / 'smart_power_socket/zephyr/.config', 'CONFIG_MCUBOOT_SIGNATURE_KEY_FILE')
    trusted = [bytes.fromhex(key['der']) for key in trusted_build_keys(build)]
    selected = public_der(app_key)
    if public_der(boot_key) not in trusted or selected not in trusted:
        raise ValueError('Configured boot/signing key is outside the project trust set')
    return boot_key, app_key, hashlib.sha256(selected).hexdigest()


def verify_compiled_trust(build):
    """Read MCUboot's linked key table, including builds where LTO folds count."""
    from elftools.elf.elffile import ELFFile
    expected = [bytes.fromhex(key['der']) for key in trusted_build_keys(build)]
    with (build / 'mcuboot/zephyr/zephyr.elf').open('rb') as source:
        elf = ELFFile(source)
        symbols = elf.get_section_by_name('.symtab')
        tables = symbols.get_symbol_by_name('bootutil_keys') if symbols else None
        pointer_size = elf.elfclass // 8
        if not tables or tables[0]['st_size'] != 2 * 2 * pointer_size:
            raise ValueError('Linked MCUboot must contain exactly two trusted key entries')

        def read(address, size):
            for segment in elf.iter_segments():
                offset = address - segment['p_vaddr']
                if segment['p_type'] == 'PT_LOAD' and 0 <= offset and offset + size <= segment['p_filesz']:
                    return segment.data()[offset:offset + size]
            raise ValueError('MCUboot trust table references data outside the ELF')

        table = read(tables[0]['st_value'], tables[0]['st_size'])
        order = 'little' if elf.little_endian else 'big'
        actual = []
        for index in range(2):
            row = table[index * 2 * pointer_size:(index + 1) * 2 * pointer_size]
            key_address = int.from_bytes(row[:pointer_size], order)
            len_address = int.from_bytes(row[pointer_size:], order)
            length = int.from_bytes(read(len_address, 4), order)
            if length != 44:
                raise ValueError('Unexpected Ed25519 DER length in linked MCUboot')
            actual.append(read(key_address, length))
        if actual != expected:
            raise ValueError('Linked MCUboot trust table differs from project public keys')
    app = (build / 'smart_power_socket/zephyr/zephyr.signed.bin').read_bytes()
    if any(key not in app for key in expected):
        raise ValueError('Application image is missing a project public key')
    return expected


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path, nargs='?', default=Path('build-ota'))
    parser.add_argument('--check-signing-key', type=Path)
    parser.add_argument('--trusted-key', type=Path, action='append', default=[])
    args = parser.parse_args()
    if args.check_signing_key:
        fingerprint = check_signer(args.check_signing_key, args.trusted_key)
        print(f'Selected signer public-key SHA-256: {fingerprint}')
    else:
        boot, app, fingerprint = inspect_build(args.build_dir)
        verify_compiled_trust(args.build_dir)
        print(f'MCUboot public key file: {boot}')
        print(f'Application signing key file: {app}')
        print(f'Selected signer public-key SHA-256: {fingerprint}')
        for key in trusted_build_keys(args.build_dir):
            print(f'Trusted {key["name"]}: {key["sha256"]}')
        print('Both project keys are trusted by the app and MCUboot; compare fingerprints on other machines.')
