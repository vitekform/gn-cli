import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('check_binary_arch', Path(__file__).resolve().parents[1] / 'scripts/check_binary_arch.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def elf(machine, bits, endian, flags=0):
    order = '<' if endian == 1 else '>'
    data = bytearray(64)
    data[:6] = b'\x7fELF' + bytes([bits, endian])
    struct.pack_into(order + 'H', data, 18, machine)
    struct.pack_into(order + 'I', data, 36, flags)
    return bytes(data)


def pe(machine):
    data = bytearray(0x200)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3C, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<H', data, 0x84, machine)
    return bytes(data)


def fat(*cputypes):
    data = bytearray(b'\xca\xfe\xba\xbe' + struct.pack('>I', len(cputypes)))
    for cpu in cputypes:
        data += struct.pack('>IIIII', cpu, 0, 0, 0, 0)
    return bytes(data)


def thin(cputype):
    return b'\xcf\xfa\xed\xfe' + struct.pack('<I', cputype) + bytes(24)


class CheckBinaryArchTest(unittest.TestCase):
    def check(self, data, platform):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(data)
        try:
            module.main(f.name, platform)
        finally:
            Path(f.name).unlink()

    def test_matching_binaries_pass(self):
        cases = [
            (elf(62, 2, 1), 'linux-x64'), (elf(183, 2, 1), 'linux-arm64'),
            (elf(40, 1, 1, 0x05000400), 'linux-armv7'), (elf(243, 2, 1), 'linux-riscv64'),
            (elf(21, 2, 1), 'linux-ppc64le'), (elf(22, 2, 2), 'linux-s390x'),
            (pe(0x8664), 'windows-x64'), (pe(0xAA64), 'windows-arm64'), (pe(0x14C), 'windows-x86'),
            (fat(0x0100000C, 0x01000007), 'macos-universal'), (thin(0x0100000C), 'macos-arm64'),
        ]
        for data, platform in cases:
            with self.subTest(platform=platform):
                self.check(data, platform)

    def test_mismatches_fail(self):
        cases = [
            (elf(62, 2, 1), 'linux-arm64'),
            (elf(40, 1, 1, 0x05000200), 'linux-armv7'),  # soft-float armel is not armhf
            (elf(21, 2, 2), 'linux-ppc64le'),            # big-endian ppc64
            (pe(0x8664), 'windows-x86'),
            (thin(0x0100000C), 'macos-universal'),       # only half of the universal binary
            (fat(0x0100000C, 0x01000007), 'linux-x64'),
        ]
        for data, platform in cases:
            with self.subTest(platform=platform), self.assertRaises(SystemExit):
                self.check(data, platform)


if __name__ == '__main__':
    unittest.main()
