#!/usr/bin/env python3
"""Verify build artifacts without touching a connected board. Use SDK Python."""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import yaml
from intelhex import IntelHex
from ota_key_info import inspect_build, verify_compiled_trust

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build_dir', type=Path, nargs='?', default=Path('build-ota'))
args = p.parse_args()
build = args.build_dir.resolve()
parts = yaml.safe_load((build/'partitions.yml').read_text())
for name, address, size, region in [
    ('mcuboot', 0, 0x20000, 'flash_primary'),
    ('mcuboot_primary', 0x20000, 0x100000, 'flash_primary'),
    ('app', 0x20800, 0xff800, 'flash_primary'),
    ('mcuboot_secondary', 0, 0x100000, 'external_flash'),
    ('ota_image_b', 0x100000, 0x100000, 'external_flash'),
    ('mcuboot_scratch', 0x200000, 0x2000, 'external_flash'),
    ('nvs_storage', 0x202000, 0x2000, 'external_flash'),
    ('settings_storage', 0x400000, 0x10000, 'external_flash'),
    ('data_storage', 0x410000, 0x3f0000, 'external_flash'),
]:
    actual = parts[name]
    assert (actual['address'], actual['size'], actual['region']) == (address, size, region), name
extents = sorted((v['address'], v['address']+v['size'], k) for k,v in parts.items()
                 if v['region']=='external_flash' and 'span' not in v and v['size'])
for start, end, name in extents:
    assert 0 <= start < end <= 0x800000, name
for left, right in zip(extents, extents[1:]):
    assert left[1] <= right[0], (left, right)
assert all(v['size'] >= 0 for v in parts.values())

def config(path):
    return dict(line.split('=',1) for line in path.read_text().splitlines() if line.startswith('CONFIG_') and '=' in line)
app = build/'smart_power_socket/zephyr'
boot = build/'mcuboot/zephyr'
ac = config(app/'.config')
bc = config(boot/'.config')
assert bc.get('CONFIG_FPROTECT') != 'y'
assert bc.get('CONFIG_NCS_MCUBOOT_DISABLE_SELF_RWX') != 'y'
assert bc['CONFIG_BOOT_SIGNATURE_TYPE_ED25519'] == 'y'
boot_image = (boot/'zephyr.bin').read_bytes()
assert len(boot_image) <= parts['mcuboot']['size']
assert b'Protect mcuboot flash failed' not in boot_image, 'Disabled protection branch is still present'
print('PASS: 128 KiB bootloader reservation, self-protection disabled, Ed25519 verification enabled')
for k in ['BOOT_GO_HOOKS', 'BOOT_VALIDATE_SLOT0', 'BOOT_SWAP_USING_SCRATCH', 'BOOT_WATCHDOG_FEED', 'WATCHDOG', 'MULTITHREADING']:
    assert bc['CONFIG_'+k] == 'y', k
assert ac['CONFIG_BOOTLOADER_MCUBOOT']=='y'
assert ac['CONFIG_SETTINGS_NVS']=='y'
assert ac['CONFIG_SPS_HTTP_OTA']=='y'
for setting in ('JSON_LIBRARY', 'PSA_WANT_ALG_SHA_512', 'PSA_WANT_ALG_PURE_EDDSA', 'PSA_WANT_ECC_TWISTED_EDWARDS_255'):
    assert ac['CONFIG_'+setting]=='y', setting
assert ac['CONFIG_FLASH_AREA_CHECK_INTEGRITY_PSA']=='y'
for setting in ('TASK_WDT', 'TASK_WDT_HW_FALLBACK'):
    assert ac['CONFIG_'+setting]=='y', setting
assert int(ac['CONFIG_TASK_WDT_MIN_TIMEOUT']) + int(ac['CONFIG_TASK_WDT_HW_FALLBACK_DELAY']) == 20000
assert bc.get('CONFIG_BOOT_DOWNGRADE_PREVENTION') != 'y'
assert bc.get('CONFIG_MCUBOOT_HW_ROLLBACK_PROT') != 'y'
# Keep existing settings readable: the private boot record has its own CRC.
assert ac.get('CONFIG_NVS_DATA_CRC') != 'y'
print('PASS: partition boundaries, settings address and boot configurations')
_, _, key_fingerprint = inspect_build(build)
print(f'PASS: selected signer is trusted; public-key SHA-256 {key_fingerprint}')
verify_compiled_trust(build)
print('PASS: linked MCUboot and application contain both project public keys')

image = app/'zephyr.signed.bin'
b = image.read_bytes()
magic, load, hsize, protected, body, flags = struct.unpack_from('<IIHHII', b)
assert magic==0x96f3b83d and hsize==0x800 and flags==0
pos = hsize+body+protected
tlv_magic, tlv_size = struct.unpack_from('<HH',b,pos)
assert tlv_magic==0x6907 and pos+tlv_size==len(b)
assert len(b)<=0xfc000
major, minor, revision, build_num = struct.unpack_from('<BBHI',b,20)
key = Path(ac['CONFIG_MCUBOOT_SIGNATURE_KEY_FILE'].strip('"'))
imgtool = key.parent/'scripts/imgtool.py'
# A custom signing key may live elsewhere: derive imgtool from the build cache.
cache = (build/'CMakeCache.txt').read_text().splitlines()
zbase = Path(next(x.split('=',1)[1] for x in cache if x.startswith('ZEPHYR_BASE:')))
imgtool = zbase.parent/'bootloader/mcuboot/scripts/imgtool.py'

def run(*cmd, ok=True):
    r = subprocess.run([sys.executable,str(imgtool),*map(str,cmd)],capture_output=True,text=True)
    assert (r.returncode==0)==ok, r.stdout+r.stderr
run('verify','-k',key,image)
with tempfile.TemporaryDirectory(prefix='sps-ota-verify-') as tmp:
    tmp = Path(tmp)
    tampered = bytearray(b)
    tampered[hsize+64] ^= 1
    corrupt = tmp/'corrupt.bin'
    corrupt.write_bytes(tampered)
    run('verify','-k',key,corrupt,ok=False)
    wrong_key = tmp/'wrong.pem'
    run('keygen','-t','ed25519','-k',wrong_key)
    run('verify','-k',wrong_key,image,ok=False)
print('PASS: real signed image accepted; tampered image and wrong key rejected')

merged = IntelHex(str(build/'merged.hex'))
assert bytes(merged.tobinarray(start=0x20000, size=len(b)))==b
for vector in (0, 0x20800):
    sp, pc = struct.unpack('<II',bytes(merged.tobinarray(start=vector,size=8)))
    assert 0x20000000 <= sp <= 0x2002f000 and pc&1, (vector,sp,pc)
assert (int.from_bytes(bytes(merged.tobinarray(start=0,size=8))[4:],'little')&~1)<0x20000
print('PASS: merged factory image contains bootloader at 0 and application at 0x20800')
manifest = {
    'board':'bl54l15_dvk/nrf54l15/cpuapp',
    'version':f'{major}.{minor}.{revision}+{build_num}',
    'filename':image.name,
    'size':len(b),
    'sha256':hashlib.sha256(b).hexdigest(),
}
(build/'ota_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'OTA image: {len(b)} bytes; version {manifest["version"]}')
print(f'Manifest: {build/"ota_manifest.json"}')

subprocess.run([sys.executable, str(Path(__file__).with_name('prepare_ota_folder.py')), str(build)], check=True)
