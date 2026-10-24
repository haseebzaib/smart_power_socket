#!/usr/bin/env python3
"""Extract the standard two-file DFU release folder, checking it against this build."""
import argparse
import json
import re
import struct
import tempfile
import zipfile
from pathlib import Path


def source_version(path):
    text = path.read_text()
    values = []
    for field, limit in [('VERSION_MAJOR', 255), ('VERSION_MINOR', 255),
                         ('PATCHLEVEL', 65535), ('VERSION_TWEAK', 0xffffffff)]:
        match = re.search(rf'^{field}\s*=\s*([0-9]+)\s*$', text, re.MULTILINE)
        if not match or int(match[1]) > limit:
            raise ValueError(f'Invalid {field} in {path}')
        values.append(int(match[1]))
    extra = re.search(r'^EXTRAVERSION\s*=([^\r\n]*)$', text, re.MULTILINE)
    if not extra or extra[1].strip():
        raise ValueError('SPS OTA releases require EXTRAVERSION to be empty')
    return '{}.{}.{}+{}'.format(*values)


def prepare(build, output, version_file=None):
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
    app_header = build / 'smart_power_socket/zephyr/include/generated/zephyr/app_version.h'
    match = re.search(r'^#define APP_VERSION_TWEAK_STRING\s+"([^"]+)"',
                      app_header.read_text(), re.MULTILINE)
    if not match or match[1] != version:
        raise ValueError('Compiled application version differs from the signed image; reconfigure/rebuild')
    if version_file and source_version(version_file) != version:
        raise ValueError(f'Build version {version} differs from {version_file}; reconfigure/rebuild')
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
    parser.add_argument('--version-file', type=Path,
                        help='Require the built image to match this source VERSION file')
    args = parser.parse_args()
    prepare(args.build_dir, args.output or args.build_dir / 'dfu_application', args.version_file)
