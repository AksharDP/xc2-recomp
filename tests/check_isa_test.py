"""Instruction boundaries used to audit baseline and v2 CPU builds."""
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_isa import classify, find_objdump, scan


class InstructionLevelTest(unittest.TestCase):
    def test_baseline_sse2(self):
        for asm in ('movdqa %xmm0,%xmm1', 'paddq %xmm0,%xmm1', 'pextrw $0,%xmm0,%eax',
                    'lock cmpxchgq %rax,(%rdx)', 'rep bsf %rax,%rax'):
            with self.subTest(asm=asm):
                self.assertIsNone(classify(asm, 1))

    def test_baseline_excludes_v2_additions(self):
        for asm in ('movddup %xmm0,%xmm1', 'pshufb %xmm0,%xmm1', 'pmulld %xmm0,%xmm1',
                    'pcmpgtq %xmm0,%xmm1', 'crc32q %rax,%rcx', 'popcntq %rax,%rcx',
                    'lock cmpxchg16b (%rcx)', 'lahf', 'sahf', 'fisttpl (%rcx)',
                    'pextrw $0,%xmm0,(%rax)'):
            with self.subTest(asm=asm):
                self.assertEqual(classify(asm, 1), 'above')

    def test_v2_instructions_stay_allowed_at_v2(self):
        for asm in ('movddup %xmm0,%xmm1', 'pshufb %xmm0,%xmm1', 'pmulld %xmm0,%xmm1',
                    'popcntq %rax,%rcx', 'lock cmpxchg16b (%rcx)', 'lahf'):
            with self.subTest(asm=asm):
                self.assertIsNone(classify(asm, 2))

    def test_v3_and_optional_instructions(self):
        for level in (1, 2):
            self.assertEqual(classify('vaddps %ymm0,%ymm1,%ymm2', level), 'above')
            self.assertEqual(classify('sha256rnds2 %xmm0,%xmm1', level), 'optional')
            self.assertEqual(classify('aesenc %xmm0,%xmm1', level), 'optional')

    def test_compiled_instruction_boundaries(self):
        cc = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        cc = str(cc) if cc.is_file() else shutil.which('clang')
        objdump = find_objdump(None)
        if not cc or not objdump:
            self.skipTest('clang and objdump are needed for the object scan')
        with tempfile.TemporaryDirectory() as tmp:
            src, obj = Path(tmp) / 'fixture.s', Path(tmp) / 'fixture.o'
            src.write_text('''.text
.globl baseline_ok
baseline_ok:
    movdqa %xmm0, %xmm1
    ret
.globl requires_sse3
requires_sse3:
    movddup %xmm0, %xmm1
    ret
.globl requires_popcnt
requires_popcnt:
    popcnt %rax, %rcx
    ret
''')
            subprocess.run([cc, '-c', '-target', 'x86_64-w64-windows-gnu', str(src), '-o', str(obj)],
                           check=True, capture_output=True)
            baseline, count = scan(objdump, obj, 1)
            self.assertGreater(count, 0)
            self.assertEqual({fn for fn, _ in baseline}, {'requires_sse3', 'requires_popcnt'})
            self.assertEqual(dict(scan(objdump, obj, 2)[0]), {})


if __name__ == '__main__':
    unittest.main()
