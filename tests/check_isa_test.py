"""Instruction boundaries used to audit baseline and v2 CPU builds."""
import collections
import contextlib
import io
import json
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_game  # noqa: E402
import check_isa  # noqa: E402
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
            # zstd's static *_bmi2 functions carry the name of whatever global precedes them in the executable
            ('vpx_tree_merge_probs', 'fse_decompress.c.obj', {'shrx', 'shlx', 'andn', 'bzhi', 'lzcnt'}),
            ('ZSTD_decompressBlock', 'huf_decompress.c.obj', {'shlxq', 'shrxq', 'lzcntl'}),
        ]
        for name, obj, ops in cases:
            with self.subTest(name=name):
                self.assertIsNotNone(reviewed_dispatch(name, obj, ops))
                self.assertIsNone(reviewed_dispatch(name, 'unrelated.o', ops))
                self.assertIsNone(reviewed_dispatch(name, obj, ops | {'movddup'}))
        self.assertIsNone(reviewed_dispatch('ZSTD_buildFSETable', 'huf_decompress.o', {'shlxq'}))
        self.assertIsNone(reviewed_dispatch('_ZNSt11range_errorD2Ev', 'random.o', {'rdrandl'}))

    def test_zstd_objects_excuse_bmi_only(self):
        # BMI2_TARGET_ATTRIBUTE is lzcnt,bmi,bmi2: AVX, MOVBE and ADX in these objects are not behind zstd's check
        for ops in ({'shlx', 'vpaddd'}, {'shrx', 'movbe'}, {'adcx'}, {'vpbroadcastd'}):
            with self.subTest(ops=ops):
                self.assertIsNone(reviewed_dispatch('HUF_decompress4X_hufOnly_wksp', 'entropy_common.c.obj', ops))
        # Other zstd objects hold no BMI2_TARGET_ATTRIBUTE decoder code (the encoder's are not linked)
        for obj in ('zstd_decompress.c.obj', 'huf_compress.c.obj', 'zstd_compress_sequences.c.obj', 'libzstd.a'):
            with self.subTest(obj=obj):
                self.assertIsNone(reviewed_dispatch('ZSTD_decompressMultiFrame', obj, {'shlx'}))
        self.assertIsNotNone(reviewed_dispatch('x', 'zstd_decompress_block.c.obj', {'blsr', 'pdep', 'rorx'}))

    def test_linked_static_function_is_attributed_to_its_object(self):
        """A static function the executable keeps no symbol for: the map's object decides, not the label."""
        cc = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        cc = str(cc) if cc.is_file() else shutil.which('clang')
        objdump = find_objdump(None)
        if not cc or not objdump:
            self.skipTest('clang and objdump are needed for the linked scan')
        # The instructions after each ret stand for a static function: with no symbol they disassemble under the
        # global before them
        sources = {
            'other.o': ('entry', 'shrx %rax, %rbx, %rcx'),
            'huf_decompress.c.obj': ('HUF_entry', 'shlx %rax, %rbx, %rcx\n    lzcnt %rax, %rcx'),
            'fse_decompress.c.obj': ('FSE_entry', 'shlx %rax, %rbx, %rcx\n    vpaddd %ymm0, %ymm1, %ymm2'),
        }
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for obj, (name, body) in sources.items():
                src = tmp / f'{name}.s'
                src.write_text(f'.text\n.globl {name}\n{name}:\n    ret\n    {body}\n    ret\n')
                subprocess.run([cc, '-c', '-target', 'x86_64-w64-windows-gnu', str(src), '-o', str(tmp / obj)],
                               check=True, capture_output=True)
            exe = tmp / 'fixture.exe'
            link = subprocess.run([cc, '-target', 'x86_64-w64-windows-gnu', '-nostdlib', '-fuse-ld=lld',
                                   '-Wl,-e,entry', f'-Wl,--Map={tmp / "fixture.map"}', *(str(tmp / o) for o in sources),
                                   '-o', str(exe)], capture_output=True, text=True)
            if link.returncode:
                self.skipTest(f'clang could not link a PE fixture with lld: {link.stderr.strip()[-200:]}')
            out = tmp / 'report.json'
            run = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'tools' / 'check_isa.py'),
                                  str(exe), '--level', '2', '--objdump', objdump, '--json', str(out)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
            report = json.loads(out.read_text(encoding='utf-8'))[str(exe)]
            self.assertEqual(set(report['dispatched']), {'HUF_entry [huf_decompress.c.obj]'})
            self.assertEqual(set(report['unexpected']), {'entry [other.o]', 'FSE_entry [fse_decompress.c.obj]'})

    def test_a_label_does_not_reach_into_the_next_object(self):
        """plain.o's static function has no symbol and follows vpx_copy_avx2 of another object in the disassembly."""
        listing = ('140001000 <vpx_copy_avx2>:\n140001000: vmovdqu (%rax), %ymm0\n140001008: vpaddd %ymm0, %ymm1, %ymm2\n'
                   '140001010: vpaddd %ymm0, %ymm1, %ymm2\n140001020 <plain>:\n140001020: vaddps %ymm0, %ymm1, %ymm2\n')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fixture.map'
            path.write_text('00001000 00000010    16         kernels.c.obj:(.text)\n'
                            '00001010 00000020    16         plain.o:(.text)\n')
            proc = mock.Mock(stdout=io.StringIO(listing), stderr=io.StringIO(''), wait=lambda: 0)
            with mock.patch.object(check_isa.subprocess, 'Popen', return_value=proc):
                found, total = scan('objdump', 'x.exe', 2, LinkMap(path, 0x140000000))
        self.assertEqual(total, 4)
        self.assertEqual({k: sum(v.values()) for k, v in found.items()},
                         {('vpx_copy_avx2', 'kernels.c.obj'): 2, ('?', 'plain.o'): 1, ('plain', 'plain.o'): 1})
        self.assertIsNotNone(check_isa.dispatched('vpx_copy_avx2', 'kernels.c.obj'))
        self.assertIsNone(check_isa.dispatched('?', 'plain.o'))

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

    def test_v3_vector_instructions_stay_allowed_at_v3(self):
        for asm in ('vaddps %ymm0,%ymm1,%ymm2', 'vfmadd231sd %xmm0,%xmm1,%xmm2', 'vfnmsub132pd (%rax),%ymm1,%ymm2',
                    'vpermilps $0x1b,%xmm0,%xmm1', 'vpermq $0xd8,%ymm0,%ymm1', 'vpackuswb %xmm0,%xmm1,%xmm2',
                    'vcvtpd2psx (%rax),%xmm0', 'vcvttsd2si %xmm0,%rax', 'vcvtsi2sdl (%rax),%xmm1,%xmm1',
                    'vcmpunordpd %ymm0,%ymm1,%ymm2', 'vpgatherdd %ymm0,(%rax,%ymm1,4),%ymm2', 'vbroadcastf128 (%rax),%ymm0',
                    'vpsravd %ymm0,%ymm1,%ymm2', 'vpmovzxbw (%rax),%ymm0', 'vzeroupper', 'vmovq %xmm0,%rax',
                    'mulx %rax,%rcx,%rdx', 'movbe (%rax),%ecx', 'lzcnt %rax,%rcx', 'verw %ax'):
            with self.subTest(asm=asm):
                self.assertIsNone(classify(asm, 3))

    def test_avx512_and_adx_are_above_v3(self):
        for asm in ('vpternlogd $0,%xmm0,%xmm1,%xmm2', 'vpabsq %ymm0,%ymm1', 'vpsraq $3,%xmm0,%xmm1',
                    'vaddps %xmm16,%xmm1,%xmm2', 'vaddps (%rax){1to4},%xmm1,%xmm2', 'vaddps %zmm0,%zmm1,%zmm2',
                    'vmovdqu8 %ymm0,%ymm1', 'vpcmpeqd %xmm0,%xmm1,%k1', 'kmovw %k0,%eax',
                    '{vex} vpdpbusd %ymm0,%ymm1,%ymm2', 'adcx %rax,%rcx', 'adoxq %rax,%rcx'):
            with self.subTest(asm=asm):
                self.assertEqual(classify(asm, 3), 'above')
        for asm in ('vaesenc %xmm0,%xmm1,%xmm2', 'vpclmulqdq $0,%xmm0,%xmm1,%xmm2', 'sha256rnds2 %xmm0,%xmm1'):
            with self.subTest(asm=asm):
                self.assertEqual(classify(asm, 3), 'optional')

    def test_runtime_audit_refuses_extensions_outside_the_levels(self):
        """build_game.py check_runtime_isa: AES without a known CPU check stops the build, sha256.c's checked SHA does not."""
        def audit(counts):
            with mock.patch.object(check_isa, 'scan', lambda *a: ({k: collections.Counter(v) for k, v in counts.items()}, 1)), \
                    mock.patch.object(check_isa, 'find_objdump', lambda hint: 'objdump'), \
                    contextlib.redirect_stdout(io.StringIO()):
                build_game.check_runtime_isa([Path('x.o')])
        audit({('blocks_ni', None): [('optional', 'sha256rnds2')]})
        for kind, mnemonic in (('optional', 'aesenc'), ('above', 'vaddps')):
            with self.subTest(mnemonic=mnemonic), self.assertRaisesRegex(SystemExit, mnemonic):
                audit({('plain', None): [(kind, mnemonic)]})

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
