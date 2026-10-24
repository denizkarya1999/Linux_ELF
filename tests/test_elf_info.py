"""Exercise ELF layouts and malformed inputs without running inspected files."""
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else './elf_info').resolve())


def fixture(bits=64, order='<'):
    """Construct a section-free ELF whose virtual addresses differ from offsets."""
    data = bytearray(1024)
    data[:16] = b'\x7fELF' + bytes([2 if bits == 64 else 1, 1 if order == '<' else 2, 1]) + bytes(9)
    is64 = bits == 64
    header_size, phsize = (64, 56) if is64 else (52, 32)
    fields = (3, 62 if is64 else 3, 1, 0x4000, header_size, 0, 0,
              header_size, phsize, 3, 0, 0, 0)
    struct.pack_into(order + ('HHIQQQIHHHHHH' if is64 else 'HHIIIIIHHHHHH'), data, 16, *fields)
    strings = b'\0libexample.so\0libsecond.so\0'
    interpreter = b'/fixture/ld.so\0'
    dynamic = [(1, 1), (5, 0x4180), (10, len(strings)), (1, 15), (0, 0)]
    stride = 16 if is64 else 8
    segments = [(1, 0x100, 0x4000, 0x300, 0x320),
                (2, 0x200, 0x4100, len(dynamic) * stride, len(dynamic) * stride),
                (3, 0x300, 0x4200, len(interpreter), len(interpreter))]
    for i, (kind, offset, address, size, memory) in enumerate(segments):
        values = ((kind, 4, offset, address, 0, size, memory, 1) if is64
                  else (kind, offset, address, 0, size, memory, 4, 1))
        struct.pack_into(order + ('IIQQQQQQ' if is64 else 'IIIIIIII'),
                         data, header_size + i * phsize, *values)
    for i, entry in enumerate(dynamic):
        struct.pack_into(order + ('QQ' if is64 else 'II'), data, 0x200 + i * stride, *entry)
    data[0x280:0x280 + len(strings)] = strings
    data[0x300:0x300 + len(interpreter)] = interpreter
    return data


class ElfInfoTests(unittest.TestCase):
    def run_file(self, data, success=True):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.elf'
            path.write_bytes(data)
            result = subprocess.run([BINARY, str(path)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0 if success else 1, result.stderr)
        if not success:
            self.assertIn('elf_info:', result.stderr)
        return result

    def test_class_and_byte_order(self):
        for bits in (32, 64):
            for order in ('<', '>'):
                with self.subTest(bits=bits, order=order):
                    output = self.run_file(fixture(bits, order)).stdout
                    self.assertIn(f'Class:        ELF{bits}', output)
                    self.assertIn('Little-endian' if order == '<' else 'Big-endian', output)
                    self.assertIn('/fixture/ld.so', output)
                    self.assertIn('  libexample.so\n  libsecond.so\n', output)

    def test_no_program_headers(self):
        data = fixture()
        struct.pack_into('<H', data, 16, 1)  # ET_REL, not a static executable.
        struct.pack_into('<Q', data, 32, 0)
        struct.pack_into('<H', data, 56, 0)
        output = self.run_file(data[:64]).stdout
        self.assertIn('Relocatable object', output)
        self.assertIn('(no dynamic segment)', output)

    def test_truncated_files(self):
        for size in (0, 1, 4, 15, 16, 32, 51, 63, 64, 127, 231, 512, 1023):
            with self.subTest(size=size):
                self.run_file(fixture()[:size], success=False)

    def test_invalid_header_fields(self):
        for offset, fmt, value in ((0, 'I', 0), (4, 'B', 9), (5, 'B', 9),
                                   (6, 'B', 0), (20, 'I', 0), (52, 'H', 0),
                                   (54, 'H', 1), (56, 'H', 65535),
                                   (32, 'Q', 2**64 - 1)):
            with self.subTest(offset=offset):
                data = fixture()
                struct.pack_into('<' + fmt, data, offset, value)
                self.run_file(data, success=False)

    def test_invalid_segment_and_string_ranges(self):
        mutations = [
            (64 + 8, 2**64 - 1),  # Load segment offset overflows naive addition.
            (64 + 32, 2**64 - 1),  # Load segment file size.
            (64 + 40, 1),  # File size exceeds memory size.
            (0x200 + 8, 999),  # DT_NEEDED outside the string table.
            (0x200 + 16 + 8, 0xffff),  # DT_STRTAB not mapped by PT_LOAD.
            (0x200 + 32 + 8, 2**64 - 1),  # DT_STRSZ cannot fit.
            (64 + 56 + 32, 79),  # Misaligned dynamic segment size.
            (0x200 + 64, 1),  # No DT_NULL terminator.
        ]
        for offset, value in mutations:
            with self.subTest(offset=offset, value=value):
                data = fixture()
                struct.pack_into('<Q', data, offset, value)
                self.run_file(data, success=False)

    def test_unterminated_strings(self):
        for start, size in ((0x300, 15), (0x280, 28)):
            with self.subTest(start=start):
                data = fixture()
                data[start:start + size] = b'X' * size
                self.run_file(data, success=False)

    def test_duplicate_segments(self):
        for kind in (2, 3):
            data = fixture()
            struct.pack_into('<I', data, 64, kind)
            self.run_file(data, success=False)

    def test_terminal_escapes_are_not_printed_raw(self):
        data = fixture()
        data[0x301] = 27
        self.assertIn('\\x1b', self.run_file(data).stdout)

    def test_large_file_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'large.elf'
            with path.open('wb') as file:
                file.truncate(256 * 1024 * 1024 + 1)  # Sparse file, no large allocation.
            result = subprocess.run([BINARY, str(path)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn('256 MiB', result.stderr)

    def test_cli(self):
        self.assertEqual(subprocess.run([BINARY, '--help'], capture_output=True).returncode, 0)
        self.assertEqual(subprocess.run([BINARY], capture_output=True).returncode, 1)
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([BINARY, os.path.join(directory, 'missing')], capture_output=True)
            self.assertEqual(result.returncode, 1)

    @unittest.skipUnless(shutil.which('readelf'), 'readelf is needed for reference comparison')
    def test_real_binaries_against_readelf(self):
        directory = Path(BINARY).parent
        files = [Path(BINARY)] + [directory / name for name in
                                 ('kernel_info', 'elf_info_android', 'kernel_info_android')
                                 if (directory / name).exists()]
        for path in files:
            with self.subTest(path=path.name):
                actual = subprocess.check_output([BINARY, str(path)], text=True)
                reference = subprocess.check_output(['readelf', '-W', '-h', '-l', '-d', str(path)], text=True)
                libraries = re.findall(r'\(NEEDED\).*?\[(.*?)\]', reference)
                self.assertEqual(re.findall(r'^  (.+)$', actual, re.M), libraries)
                for interpreter in re.findall(r'Requesting program interpreter: (.*?)\]', reference):
                    self.assertIn('Interpreter:  ' + interpreter, actual)
                elf_class = re.search(r'Class:\s+(ELF\d+)', reference).group(1)
                self.assertIn('Class:        ' + elf_class, actual)


if __name__ == '__main__':
    unittest.main()
