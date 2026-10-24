#!/usr/bin/env python3
"""Extract the standard two-file DFU release folder, checking it against this build."""
import argparse
import json
import re
import struct
import tempfile
import zipfile
from pathlib import Path


def prepare(build, output):
    with zipfile.ZipFile(build / 'dfu_application.zip') as archive:
        manifest_bytes = archive.read('manifest.json')
        manifest = json.loads(manifest_bytes)
        if manifest.get('format-version') != 1 or manifest.get('name') != 'smart_power_socket':
            raise ValueError('Unexpected DFU manifest format/name')
        files = manifest.get('files', [])
        if len(files) != 1:
            raise ValueError('Expected exactly one application image')
        entry = files[0]
        filename = entry['file']
        if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*\.bin', filename):
            raise ValueError('Unsafe DFU filename')
        if (entry.get('type'), entry.get('board'), entry.get('soc'), entry.get('load_address'),
            entry.get('image_index'), entry.get('slot_index_primary'), entry.get('slot_index_secondary')) != (
                'application', 'bl54l15_dvk', 'nrf54l15', 0x20800, '0', '1', '2'):
            raise ValueError('DFU package targets another board/layout')
        payload = archive.read(filename)
    if payload != (build / 'smart_power_socket/zephyr/zephyr.signed.bin').read_bytes():
        raise ValueError('DFU ZIP is stale or differs from this build')
    if len(payload) != entry['size'] or not 32 <= len(payload) <= 0xfc000:
        raise ValueError('DFU size mismatch or slot overflow')
    magic, load, header, protected, body, flags = struct.unpack_from('<IIHHII', payload)
    version = '{}.{}.{}+{}'.format(*struct.unpack_from('<BBHI', payload, 20))
    if magic != 0x96f3b83d or header != 0x800 or flags != 0 or version != entry['version_MCUBOOT']:
        raise ValueError('DFU header/version mismatch')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sps-release-', dir=output) as temp:
        temp = Path(temp)
        (temp / filename).write_bytes(payload)
        (temp / 'manifest.json').write_bytes(manifest_bytes)
        (temp / filename).replace(output / filename)
        (temp / 'manifest.json').replace(output / 'manifest.json')
    print(f'Release folder: {output} (version {version}, {len(payload)} bytes)')
    print(f'Upload manifest.json and {filename} together; keep their contents unchanged.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path, nargs='?', default=Path('build-ota'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    prepare(args.build_dir, args.output or args.build_dir / 'dfu_application')
