#!/usr/bin/env python3
"""Packaging metadata tests; firmware signature tests live in tests/http_ota."""
import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from prepare_ota_folder import prepare, source_version


class ReleasePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.output = self.build / 'dfu_application'
        self.version_file = self.build / 'VERSION'
        self.write_version((2, 0, 0, 0))
        self.image = bytearray(2056)
        struct.pack_into('<IIHHII', self.image, 0, 0x96f3b83d, 0, 0x800, 0, 4, 0)
        struct.pack_into('<BBHI', self.image, 20, 2, 0, 0, 0)
        self.entry = dict(type='application', board='bl54l15_dvk', soc='nrf54l15',
                          load_address=0x20800, image_index='0', slot_index_primary='1',
                          slot_index_secondary='2', version_MCUBOOT='2.0.0+0',
                          file='smart_power_socket.signed.bin', size=len(self.image))
        self.signed = self.build / 'smart_power_socket/zephyr/zephyr.signed.bin'
        self.signed.parent.mkdir(parents=True)
        self.signed.write_bytes(self.image)
        self.header = self.signed.parent / 'include/generated/zephyr/app_version.h'
        self.header.parent.mkdir(parents=True)
        self.header.write_text('#define APP_VERSION_TWEAK_STRING "2.0.0+0"\n')
        self.write_zip()

    def write_version(self, version):
        self.version_file.write_text('\n'.join(f'{name} = {number}' for name, number in zip(
            ['VERSION_MAJOR', 'VERSION_MINOR', 'PATCHLEVEL', 'VERSION_TWEAK'], version)) + '\nEXTRAVERSION =\n')

    def write_zip(self):
        self.manifest = json.dumps({'format-version': 1, 'name': 'smart_power_socket', 'files': [self.entry]}).encode()
        with zipfile.ZipFile(self.build / 'dfu_application.zip', 'w') as z:
            z.writestr('manifest.json', self.manifest)
            z.writestr(self.entry['file'], self.image)

    def run_prepare(self):
        with contextlib.redirect_stdout(io.StringIO()):
            prepare(self.build, self.output, self.version_file)

    def test_refreshes_stale_extracted_pair_without_rewriting_manifest(self):
        self.output.mkdir()
        (self.output / 'manifest.json').write_text('stale 1.0.1')
        (self.output / self.entry['file']).write_bytes(b'old image')
        self.run_prepare()
        self.assertEqual((self.output / 'manifest.json').read_bytes(), self.manifest)
        self.assertEqual((self.output / self.entry['file']).read_bytes(), self.image)

    def test_rejects_old_build_after_source_version_changes(self):
        self.write_version((2, 0, 1, 0))
        with self.assertRaisesRegex(ValueError, 'differs from .*VERSION'):
            self.run_prepare()
        self.assertFalse(self.output.exists())

    def test_rejects_stale_zip_payload(self):
        self.signed.write_bytes(bytes(self.image) + b'changed')
        with self.assertRaisesRegex(ValueError, 'ZIP is stale'):
            self.run_prepare()

    def test_rejects_stale_zip_manifest_version(self):
        self.entry['version_MCUBOOT'] = '1.0.1+0'
        self.write_zip()
        with self.assertRaisesRegex(ValueError, 'header/version mismatch'):
            self.run_prepare()

    def test_rejects_runtime_version_different_from_signed_version(self):
        self.header.write_text('#define APP_VERSION_TWEAK_STRING "1.0.1+0"\n')
        with self.assertRaisesRegex(ValueError, 'Compiled application version differs'):
            self.run_prepare()

    def test_source_version_handles_crlf(self):
        self.version_file.write_bytes(self.version_file.read_bytes().replace(b'\n', b'\r\n'))
        self.assertEqual(source_version(self.version_file), '2.0.0+0')

    def test_source_version_rejects_invalid_numbers_and_suffix(self):
        self.write_version((256, 0, 0, 0))
        with self.assertRaisesRegex(ValueError, 'Invalid VERSION_MAJOR'):
            source_version(self.version_file)
        self.write_version((2, 0, 0, 0))
        self.version_file.write_text(self.version_file.read_text().replace('EXTRAVERSION =', 'EXTRAVERSION = beta'))
        with self.assertRaisesRegex(ValueError, 'EXTRAVERSION'):
            source_version(self.version_file)


if __name__ == '__main__':
    unittest.main()
