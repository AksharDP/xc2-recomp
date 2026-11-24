"""Instruction boundaries used to audit baseline and v2 CPU builds."""
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_isa import classify, find_objdump, scan, LinkMap, reviewed_dispatch


class InstructionLevelTest(unittest.TestCase):
    def test_function_section_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fixture.map'
            path.write_text('00001000 00000020    16         random.o:(.text$local_rng)\n'
                            '00001020 00000010    16         other.o:(.text)\n')
            symbols = LinkMap(path, 0x140000000)
            self.assertEqual(symbols.object_at(0x140001001), 'random.o')
            self.assertEqual(symbols.symbol_at(0x140001001), 'local_rng')
            self.assertIsNone(symbols.symbol_at(0x140001020))
            self.assertIsNone(symbols.object_at(0x140001030))

    def test_elf_map(self):
        """lld's ELF map (the Linux build): addresses, not RVAs, and archive members inside the archive's name."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'xc2.map'
            path.write_text(
                '             VMA              LMA     Size Align Out     In      Symbol\n'
                '          201000           201000   900000    16 .text\n'
                '          c2e8f0           c2e8f0      aca    16         sysroot/usr/lib/libvpx.a(avg_intrin_avx2.c.o):(.text)\n'
                '          c2e8f0           c2e8f0       40     1                 vpx_hadamard_16x16_avx2\n'
                '          575cc8           575cc8       90     1         sysroot/usr/lib/libvpx.a(avg_intrin_avx2.c.o):(.eh_frame+0x18)\n'
                '          d443a0           d443a0     a922    16         runtime/deps/lib/libzstd.a(huf_decompress.o):(.text)\n'
                '          d72980           d72980       3e    16         gcc/10/libstdc++.a(random.o):(.text._ZNSt12_GLOBAL__N_112__x86_rdrandEPv)\n'
                '          e00000           e00000       10    16         build/out/runtime/main.o:(.text.main)\n')
            symbols = LinkMap(path, 0)
            self.assertEqual(symbols.object_at(0xc2e8f0 + 0x100), 'avg_intrin_avx2.c.o')
            self.assertEqual(symbols.object_at(0xd443a0), 'huf_decompress.o')
            self.assertEqual(symbols.object_at(0xd72980), 'random.o')
            self.assertEqual(symbols.object_at(0xe00004), 'main.o')
            self.assertIsNone(symbols.object_at(0x575cd0))   # not code
            self.assertIsNone(symbols.symbol_at(0xd72980))   # ELF keeps local names in its symbol table

    def test_reviewed_dispatch_is_restricted(self):
        cases = [
            ('HUF_decompress4X1_usingDTable_internal_fast', 'huf_decompress.o', {'shlxq', 'andnq'}),
            ('_HUF_decompress4X2_usingDTable_internal_fast_asm_loop', 'huf_decompress_amd64.o', {'shlxq'}),
            ('idct32_135_8x32_ssse3', 'inv_txfm_ssse3.c.o', {'pmulhrsw'}),
            ('_ZNSt12_GLOBAL__N_1L12__x86_rdrandEPv', 'random.o', {'rdrandl'}),
            ('_ZNSt12_GLOBAL__N_112__x86_rdrandEPv', 'random.o', {'rdrandl'}),   # GCC 10 (Linux release sysroot)
            ('_ZNSt12_GLOBAL__N_119__x86_rdseed_rdrandEPv', 'random.o', {'rdseedl'}),
        ]
        for name, obj, ops in cases:
            with self.subTest(name=name):
                self.assertIsNotNone(reviewed_dispatch(name, obj, ops))
                self.assertIsNone(reviewed_dispatch(name, 'unrelated.o', ops))
                self.assertIsNone(reviewed_dispatch(name, obj, ops | {'movddup'}))
        self.assertIsNone(reviewed_dispatch('ZSTD_buildFSETable', 'huf_decompress.o', {'shlxq'}))
        self.assertIsNone(reviewed_dispatch('_ZNSt11range_errorD2Ev', 'random.o', {'rdrandl'}))

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
