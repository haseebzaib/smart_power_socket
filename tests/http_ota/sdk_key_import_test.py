#!/usr/bin/env python3
"""Run linked ARM CRACEN key-import code, without replacing it with OpenSSL.

Use SDK Python with pyelftools and Unicorn installed. This executes only the
CPU key-import path; it does not emulate CRACEN signature hardware or flash.
"""
import argparse
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M33, UC_ARM_REG_R0,
                              UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                              UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)


def import_key(elf_path, key, algorithm=0x06000800):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
    cpu.mem_map(0, 0x200000)
    cpu.mem_map(0x20000000, 0x30000)
    with elf_path.open('rb') as source:
        elf = ELFFile(source)
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                cpu.mem_write(segment['p_vaddr'], segment.data())
        symbol = elf.get_section_by_name('.symtab').get_symbol_by_name('cracen_import_key')[0]
        function = symbol['st_value']

    attributes, data, output = 0x20021000, 0x20021100, 0x20021200
    length, bits, stack, end = 0x20021300, 0x20021304, 0x2002e000, 0x100000
    # NCS 3.2.4 PSA ABI: public Ed25519, infer bit count, volatile local key,
    # VERIFY_MESSAGE usage and PureEdDSA algorithm, no secondary algorithm/id.
    cpu.mem_write(attributes, struct.pack('<HH6I', 0x4142, 0, 0, 0x800, algorithm, 0, 0, 0))
    cpu.mem_write(data, key)
    cpu.mem_write(stack, struct.pack('<III', 32, length, bits))
    for register, value in ((UC_ARM_REG_R0, attributes), (UC_ARM_REG_R1, data),
                            (UC_ARM_REG_R2, len(key)), (UC_ARM_REG_R3, output),
                            (UC_ARM_REG_SP, stack), (UC_ARM_REG_LR, end | 1)):
        cpu.reg_write(register, value)
    cpu.emu_start(function | 1, end, count=200000)
    assert cpu.reg_read(UC_ARM_REG_PC) == end, 'SDK import did not return within the instruction limit'
    status = struct.unpack('<i', struct.pack('<I', cpu.reg_read(UC_ARM_REG_R0)))[0]
    size = struct.unpack('<I', cpu.mem_read(length, 4))[0]
    key_bits = struct.unpack('<I', cpu.mem_read(bits, 4))[0]
    return status, bytes(cpu.mem_read(output, min(size, 32))), key_bits


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('--old-elf', type=Path)
    args = parser.parse_args()
    keys = json.loads((args.build_dir / 'smart_power_socket/ota_generated/trusted_keys.json').read_text())['keys']
    image = args.build_dir / 'smart_power_socket/zephyr/zephyr.elf'
    for entry in keys:
        key = bytes.fromhex(entry['der'])[-32:]
        if args.old_elf:
            assert import_key(args.old_elf, key)[0] == -134
        assert import_key(image, key) == (0, key, 255)
        assert import_key(image, key[:-1])[0] == -135
        assert import_key(image, bytes(32))[0] == -135
        assert import_key(image, key, algorithm=0)[0] == -135
    print('PASS: linked NCS ARM CRACEN imports both project Ed25519 public keys; malformed keys/policies rejected')
    if args.old_elf:
        print('PASS: previous ELF reproduces PSA -134 for the identical public-key import')
