#!/usr/bin/env python3
"""Test permanent-key generation and trust-set validation without touching project keys."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from generate_ota_keys import generate
from export_ota_key import export
from ota_key_info import check_signer, public_der
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


class SigningKeysTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        with contextlib.redirect_stdout(io.StringIO()):
            generate(self.root)
        self.private = [self.root / 'private' / f'{name}-ed25519.pem' for name in ('primary', 'recovery')]
        self.public = [self.root / 'public' / f'{name}-ed25519.pem' for name in ('primary', 'recovery')]

    def test_two_independent_matching_pairs(self):
        self.assertNotEqual(public_der(self.public[0]), public_der(self.public[1]))
        for private, public in zip(self.private, self.public):
            self.assertEqual(public_der(private), public_der(public))
            self.assertEqual(check_signer(private, self.public), hashlib.sha256(public_der(public)).hexdigest())
            if os.name == 'posix':
                self.assertEqual(stat.S_IMODE(private.stat().st_mode), 0o600)

    def test_generation_cannot_replace_existing_keys(self):
        before = [p.read_bytes() for p in self.private + self.public]
        with self.assertRaisesRegex(ValueError, 'already exists'):
            generate(self.root)
        self.assertEqual(before, [p.read_bytes() for p in self.private + self.public])

    def test_missing_private_key_has_no_fallback(self):
        self.private[0].unlink()
        with self.assertRaises(FileNotFoundError):
            check_signer(self.private[0], self.public)
        self.assertTrue(check_signer(self.private[1], self.public))

    def test_untrusted_private_key_is_rejected(self):
        foreign = self.root / 'foreign.pem'
        foreign.write_bytes(Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        with self.assertRaisesRegex(ValueError, 'must match'):
            check_signer(foreign, self.public)

    def test_public_key_cannot_be_selected_as_signer(self):
        with self.assertRaises(ValueError):
            check_signer(self.public[0], self.public)

    def test_both_export_formats_have_same_public_trust_set(self):
        export(self.public, self.root / 'app.hpp', self.root / 'app.json', 'app')
        export(self.public, self.root / 'boot.c', self.root / 'boot.json', 'mcuboot')
        app = json.loads((self.root / 'app.json').read_text())
        self.assertEqual(app, json.loads((self.root / 'boot.json').read_text()))
        self.assertEqual(len(app['keys']), 2)
        for output in ['app.hpp', 'boot.c', 'app.json', 'boot.json']:
            self.assertNotIn('PRIVATE KEY', (self.root / output).read_text())
        self.assertIn('bootutil_keys[]', (self.root / 'boot.c').read_text())

    def test_duplicate_public_keys_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'distinct'):
            export([self.public[0], self.public[0]], self.root / 'app.hpp', self.root / 'app.json', 'app')


if __name__ == '__main__':
    unittest.main()
