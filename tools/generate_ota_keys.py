#!/usr/bin/env python3
"""Generate the two permanent SPS Ed25519 pairs once; never replace existing keys."""
import argparse
import hashlib
import os
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def generate(root):
    names = ('primary', 'recovery')
    expected = [root / kind / f'{name}-ed25519.pem' for kind in ('private', 'public') for name in names]
    if any(path.exists() for path in expected):
        raise ValueError('A key file already exists. Restore/verify the existing set; keys will not be regenerated.')
    (root / 'private').mkdir(parents=True, mode=0o700, exist_ok=True)
    (root / 'public').mkdir(parents=True, exist_ok=True)
    for name in names:
        key = Ed25519PrivateKey.generate()
        private = root / 'private' / f'{name}-ed25519.pem'
        public = root / 'public' / f'{name}-ed25519.pem'
        private_bytes = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption())
        with os.fdopen(os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as output:
            output.write(private_bytes)
        with public.open('xb') as output:
            output.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                                                       serialization.PublicFormat.SubjectPublicKeyInfo))
        der = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        print(f'{name}: private={private}; public={public}; public-key SHA-256={hashlib.sha256(der).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path(__file__).resolve().parents[1] / 'keys/ota')
    args = parser.parse_args()
    generate(args.directory)
