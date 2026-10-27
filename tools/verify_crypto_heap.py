"""Verify the actual Nordic PSA allocator and startup registration in an ELF."""
from elftools.elf.elffile import ELFFile


def verify_crypto_heap(config_path, elf_path):
    config = dict(line.split('=', 1) for line in config_path.read_text().splitlines()
                  if line.startswith('CONFIG_') and '=' in line)
    if config.get('CONFIG_MBEDTLS_ENABLE_HEAP') != 'y':
        raise ValueError('Nordic PSA public-key import requires CONFIG_MBEDTLS_ENABLE_HEAP=y')
    size = int(config.get('CONFIG_MBEDTLS_HEAP_SIZE', '0'), 0)
    if size < 8192:
        raise ValueError('SPS OTA requires an initialized crypto heap of at least 8192 bytes')
    with elf_path.open('rb') as source:
        elf = ELFFile(source)
        symbols = elf.get_section_by_name('.symtab')

        def symbol(name):
            matches = symbols.get_symbol_by_name(name) if symbols else None
            if not matches:
                raise ValueError(f'Crypto allocator missing from linked application: {name}')
            return matches[0]

        heap = symbol('mbedtls_heap')
        if heap['st_size'] < size:
            raise ValueError('Linked crypto heap is smaller than the configured size')
        init = symbol('__init_mbedtls_heap_init')
        if not (symbol('__init_POST_KERNEL_start')['st_value'] <= init['st_value'] <
                symbol('__init_APPLICATION_start')['st_value']):
            raise ValueError('Crypto heap initialization must run before application startup')
        section = elf.get_section(init['st_shndx'])
        offset = init['st_value'] - section['sh_addr']
        pointer_size = elf.elfclass // 8
        pointer = int.from_bytes(section.data()[offset:offset + pointer_size],
                                 'little' if elf.little_endian else 'big')
        if pointer != symbol('mbedtls_heap_init')['st_value']:
            raise ValueError('Crypto heap startup entry does not call its initializer')
        symbol('mbedtls_memory_buffer_alloc_init')
    return size
