#!/usr/bin/env python3
"""Check that a built gn-cli binary is for the platform its release name promises.

    check_binary_arch.py dist/bin/gn-cli linux-armv7
    check_binary_arch.py dist/bin/gn-cli macos-universal

Reads the ELF, PE or Mach-O header directly, so it needs nothing but Python and runs on
every CI runner. QEMU and Rosetta run almost anything, so without this a wrong compiler
or a missed flag would ship a binary under the wrong name without any test noticing.
"""
import struct
import sys

# platform -> (format, set of architectures it must contain)
EXPECTED = {
    'linux-x64': ('elf', {'x64'}),
    'linux-arm64': ('elf', {'arm64'}),
    'linux-armv7': ('elf', {'arm32'}),
    'linux-riscv64': ('elf', {'riscv64'}),
    'linux-ppc64le': ('elf', {'ppc64le'}),
    'linux-s390x': ('elf', {'s390x'}),
    'macos-arm64': ('macho', {'arm64'}),
    'macos-x64': ('macho', {'x64'}),
    'macos-universal': ('macho', {'arm64', 'x64'}),
    'windows-x64': ('pe', {'x64'}),
    'windows-arm64': ('pe', {'arm64'}),
    'windows-x86': ('pe', {'x86'}),
}


def elf(data):
    bits, endian = data[4], data[5]
    order = '<' if endian == 1 else '>'
    machine = struct.unpack_from(order + 'H', data, 18)[0]
    names = {(62, 2, 1): 'x64', (183, 2, 1): 'arm64', (40, 1, 1): 'arm32', (243, 2, 1): 'riscv64',
             (21, 2, 1): 'ppc64le', (21, 2, 2): 'ppc64', (22, 2, 2): 's390x', (3, 1, 1): 'x86'}
    arch = names.get((machine, bits, endian), f'elf-machine-{machine}/{bits * 32}bit/{"LE" if endian == 1 else "BE"}')
    if arch == 'arm32':
        # Hard-float EABI5 (armhf), what Raspberry Pi OS and Debian/Ubuntu armhf run.
        flags = struct.unpack_from('<I', data, 36)[0]
        if flags >> 24 != 5 or not flags & 0x400:
            arch = f'arm32-eabi{flags >> 24}-{"hard" if flags & 0x400 else "soft"}float'
    return {arch}


def pe(data):
    offset = struct.unpack_from('<I', data, 0x3C)[0]
    if data[offset:offset + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    return {{0x8664: 'x64', 0xAA64: 'arm64', 0x14C: 'x86'}.get(machine, f'pe-machine-{machine:#x}')}


MACHO_CPU = {0x01000007: 'x64', 0x0100000C: 'arm64', 7: 'x86', 12: 'arm32'}


def macho(data):
    magic = data[:4]
    if magic in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):  # fat (universal), big-endian header
        count = struct.unpack_from('>I', data, 4)[0]
        size = 20 if magic == b'\xca\xfe\xba\xbe' else 32
        return {MACHO_CPU.get(struct.unpack_from('>I', data, 8 + i * size)[0], 'unknown') for i in range(count)}
    if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe'):  # thin, little-endian
        return {MACHO_CPU.get(struct.unpack_from('<I', data, 4)[0], 'unknown')}
    raise ValueError('not a Mach-O file')


def identify(path):
    with open(path, 'rb') as f:
        data = f.read(4096)
    if data[:4] == b'\x7fELF':
        return 'elf', elf(data)
    if data[:2] == b'MZ':
        return 'pe', pe(data)
    return 'macho', macho(data)


def main(path, platform):
    if platform not in EXPECTED:
        sys.exit(f'unknown platform {platform}; known: {", ".join(EXPECTED)}')
    want = EXPECTED[platform]
    got = identify(path)
    print(f'{path}: {got[0]} {" + ".join(sorted(got[1]))}')
    if got != want:
        sys.exit(f'{path} should be {want[0]} {" + ".join(sorted(want[1]))} for {platform}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
