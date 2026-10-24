#!/usr/bin/env python3
"""Generate app/MCUboot trust tables from the two checked-in public PEMs only."""
import argparse
import hashlib
import json
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


def load_public(path):
    key = serialization.load_pem_public_key(path.read_bytes())
    if not isinstance(key, Ed25519PublicKey):
        raise ValueError(f'Expected an Ed25519 public PEM: {path}')
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    raw = key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return {'name': path.stem, 'der': der, 'raw': raw, 'hash': hashlib.sha512(der).digest()}


def octets(data):
    return '{' + ','.join(f'0x{byte:02x}' for byte in data) + '}'


def export(keys, output, metadata, format):
    values = [load_public(path) for path in keys]
    if len(values) != 2 or values[0]['der'] == values[1]['der']:
        raise ValueError('Exactly two distinct Ed25519 public keys are required')
    if format == 'app':
        text = ('#pragma once\n#include <array>\n#include <cstdint>\nnamespace ota_public_key {\n'
                'struct key { std::array<uint8_t,44> der; std::array<uint8_t,32> raw; std::array<uint8_t,64> hash; };\n'
                'inline constexpr std::array<key,2> trusted = {{\n')
        for key in values:
            text += '{' + ','.join(octets(key[field]) for field in ('der', 'raw', 'hash')) + '},\n'
        text += '}};\n}\n'
    else:
        text = ('/* Generated from public PEMs; no private key material. */\n'
                '#include <bootutil/sign_key.h>\n'
                '#if !defined(MCUBOOT_SIGN_ED25519) || defined(MCUBOOT_HW_KEY)\n'
                '#error "SPS trust table requires software Ed25519 key selection"\n#endif\n')
        for i, key in enumerate(values):
            text += f'static const uint8_t sps_key_{i}[] = {octets(key["der"])};\n'
            text += f'static const unsigned int sps_key_{i}_len = sizeof(sps_key_{i});\n'
        text += 'const struct bootutil_key bootutil_keys[] = {\n'
        for i in range(2):
            text += f'{{.key=sps_key_{i}, .len=&sps_key_{i}_len}},\n'
        text += '};\nconst int bootutil_key_cnt = sizeof(bootutil_keys)/sizeof(bootutil_keys[0]);\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    metadata.write_text(json.dumps({'schema': 1, 'keys': [
        {'name': key['name'], 'der': key['der'].hex(), 'sha256': hashlib.sha256(key['der']).hexdigest()}
        for key in values]}, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--key', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--format', choices=['app', 'mcuboot'], default='app')
    args = parser.parse_args()
    export(args.key, args.output, args.metadata, args.format)
