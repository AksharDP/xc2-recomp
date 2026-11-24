"""List the functions of a linked executable (or an object or archive) that hold instructions above an x86-64 level.

A build for x86-64-v2 (tools/build_game.py --cpu v2, runtime/cpu_level.h) must not run AVX, AVX2, FMA, BMI1/2, F16C,
LZCNT or MOVBE on a processor without them. -march=x86-64-v2 keeps the compiler's own code inside that set, but inline
assembly, target attributes and prebuilt libraries pass through: runtime/kernel.c kern_ap_stub saved YMM registers in
every build until it learnt the v2 form. The generated code is LTO bitcode until the link, so the linked executable is
what this reads.

Some libraries hold such instructions behind a CPU check of their own (libvpx's AVX2 kernels, zstd's BMI2 decoders,
the SHA extension path of runtime/sha256.c). DISPATCHED names them with the check; they are listed apart and do not
fail the run. Anything else fails it (exit 1). The dynamic counterpart is a run under Intel SDE with an older
processor's CPUID, which stops at the first instruction that processor lacks.

    python tools/check_isa.py build/v301-v2/xc2.exe [--level 2] [--objdump PATH] [--json OUT]

Reads the disassembly of GNU objdump or llvm-objdump (AT&T syntax) as it streams. Standard library only.
Level 1 additionally checks the SSE3, SSSE3, SSE4 and scalar additions listed in ABOVE_BASELINE. This mnemonic-based
audit is not an exhaustive CPUID decoder or proof that dispatch guards work; combine it with older-CPU execution.
"""
import argparse
import bisect
import collections
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Not VEX/EVEX encoded (or VEX without a v prefix), above x86-64-v2: BMI1/2, LZCNT, MOVBE, ADX. tzcnt is left out:
# compilers emit it (rep bsf) at any level where bsf gives the same result, and older processors run it as bsf.
ABOVE_V2 = {'andn', 'bextr', 'blsi', 'blsmsk', 'blsr', 'bzhi', 'shlx', 'shrx', 'sarx', 'rorx', 'pdep', 'pext', 'mulx',
            'lzcnt', 'movbe', 'adcx', 'adox'}
# Intel SDM volume 1, Instruction Set Summary: SSE3, SSSE3, SSE4.1 and SSE4.2. These are additions to the
# baseline SSE2 target. GNU/LLVM spell some integer instructions with an operand-size suffix (handled below).
# https://cdrdv2-public.intel.com/812382/253665-sdm-vol-1.pdf
ABOVE_BASELINE = set('''addsubpd addsubps haddpd haddps hsubpd hsubps lddqu movddup movshdup movsldup
    fisttp monitor mwait pabsb pabsw pabsd palignr phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw
    pmulhrsw pshufb psignb psignw psignd blendpd blendps blendvpd blendvps dppd dpps extractps insertps
    movntdqa mpsadbw packusdw pblendvb pblendw pcmpeqq pextrb pextrd pextrq pextrw phminposuw pinsrb
    pinsrd pinsrq pmaxsb pmaxsd pmaxud pmaxuw pminsb pminsd pminud pminuw pmuldq pmulld pmovsxbd
    pmovsxbq pmovsxbw pmovsxdq pmovsxwd pmovsxwq pmovzxbd pmovzxbq pmovzxbw pmovzxdq pmovzxwd
    pmovzxwq ptest roundpd roundps roundsd roundss pcmpestri pcmpestrm pcmpistri pcmpistrm pcmpgtq
    crc32 popcnt cmpxchg16b lahf sahf'''.split())
# PEXTRW's MMX/SSE2 register form is baseline; its SSE4.1 memory form is not.
ABOVE_BASELINE.remove('pextrw')
# In neither level: separate CPUID features a program must check for itself
OPTIONAL = {'sha1rnds4', 'sha1nexte', 'sha1msg1', 'sha1msg2', 'sha256rnds2', 'sha256msg1', 'sha256msg2', 'aesenc',
            'aesenclast', 'aesdec', 'aesdeclast', 'aesimc', 'aeskeygenassist', 'pclmulqdq', 'rdrand', 'rdseed'}
# Legacy and virtualization instructions that start with v but are not vector ones
NOT_VECTOR = {'verr', 'verw', 'vmcall', 'vmlaunch', 'vmresume', 'vmxoff', 'vmxon', 'vmread', 'vmwrite', 'vmptrld',
              'vmptrst', 'vmclear', 'vmfunc', 'vmrun', 'vmload', 'vmsave', 'vmmcall'}
PREFIXES = {'rep', 'repe', 'repz', 'repne', 'repnz', 'lock', 'notrack', 'data16', 'data32', 'addr32', 'bnd', 'cs',
            'ds', 'es', 'ss', 'fs', 'gs', 'rex', 'rex.w', 'rex.b', 'rex.x', 'rex.r', 'xacquire', 'xrelease'}
WIDE_REG = re.compile(r'%[yz]mm\d|%k[0-7]\b')
AVX512_REG = re.compile(r'%zmm\d|%k[0-7]\b')
FUNC = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')

# Function name pattern -> the check that keeps it off processors without the instructions
DISPATCHED = [
    (re.compile(r'^(vpx|vp8|vp9|vpx_dsp|vpx_scale)_.*_(avx2?|avx512|ssse3|sse4_1)'),
     'libvpx run-time CPU detection (vpx_dsp_rtcd / vp9_rtcd: x86_simd_caps)'),
    (re.compile(r'_bmi2$|_bmi2_|^HUF_.*Bmi2|^ZSTD_.*_bmi2'), 'zstd run-time BMI2 detection (ZSTD_cpuid_bmi2)'),
    (re.compile(r'^blocks_ni$'),
     'runtime/sha256.c sha256_blocks: __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1")'),
]
# Object file (from the linker map) pattern -> the check. libvpx compiles each SIMD level's kernels into objects of their
# own (<name>_avx2.c.o); their static helpers have no symbol in the executable, so the disassembly names them after the
# global symbol before them (a _c or _sse2 function of another object). The map says where the bytes came from.
DISPATCHED_OBJECTS = [
    (re.compile(r'_(avx|avx2|avx512\w*)\.c\.o$'),
     'libvpx run-time CPU detection (vpx_dsp_rtcd / vp9_rtcd: x86_simd_caps); an AVX kernel object of libvpx'),
]
# lld's map of a COFF link: RVA, size, alignment, then the input section, an archive member by its own name
# ("avg_intrin_avx2.c.o:(.text)").
MAP_ROW = re.compile(r'^([0-9a-f]{8,16}) ([0-9a-f]{8,16})\s+\d+\s{3,}(\S.*?):\((\.text[^)]*)\)\s*$')
# lld's map of an ELF link (the Linux build): VMA, LMA, size, alignment, then the input section, an archive member
# inside its archive's name ("libvpx.a(avg_intrin_avx2.c.o):(.text)").
ELF_MAP_ROW = re.compile(r'^\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S.*?):\((\.text[^)]*)\)\s*$')
ARCHIVE_MEMBER = re.compile(r'\(([^()]+)\)$')


class LinkMap:
    """The input sections of an lld map (build_game.py writes xc2.map beside the exe): address -> object file. Reads
    the COFF map of the Windows build and the ELF map of the Linux one; image_base is 0 for the latter, whose map gives
    addresses rather than RVAs (image_base())."""

    def __init__(self, path, image_base):
        self.base, rows = image_base, []
        with path.open(encoding='utf-8', errors='replace') as source:
            for line in source:
                m = MAP_ROW.match(line) or ELF_MAP_ROW.match(line)
                if m:
                    section = m.group(4)
                    symbol = section.removeprefix('.text$') if section.startswith('.text$') else None
                    name = Path(m.group(3)).name
                    member = ARCHIVE_MEMBER.search(name)
                    rows.append((int(m.group(1), 16), int(m.group(2), 16), member.group(1) if member else name, symbol))
        rows.sort()
        self.starts = [r[0] for r in rows]
        self.rows = rows

    def object_at(self, vma):
        rva = vma - self.base
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0:
            start, size, name, _ = self.rows[i]
            if start <= rva < start + size:
                return name
        return None

    def symbol_at(self, vma):
        """COFF function sections preserve local names missing from the executable's symbol table."""
        rva = vma - self.base
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0:
            start, size, _, symbol = self.rows[i]
            if start <= rva < start + size:
                return symbol
        return None


def image_base(objdump, path):
    """What the map's addresses count from: a PE image's ImageBase (lld's COFF map gives RVAs), 0 for an ELF file (its
    map gives the addresses the disassembly shows), None when neither is known."""
    with open(path, 'rb') as f:
        if f.read(4) == b'\x7fELF':
            return 0
    out = subprocess.run([objdump, '-p', str(path)], capture_output=True, text=True, errors='replace').stdout
    m = re.search(r'^ImageBase\s+([0-9a-fA-F]+)', out, re.M)
    return int(m.group(1), 16) if m else None


def find_objdump(hint):
    if hint:
        return hint if Path(hint).is_file() else None
    for name in ('llvm-objdump', 'objdump'):
        found = shutil.which(name)
        if found:
            return found
    msys = Path(r'C:\msys64\ucrt64\bin\objdump.exe')
    return str(msys) if msys.is_file() else None


def classify(text, level):
    """'above' (outside the level), 'optional' (an extension outside both levels) or None for one instruction."""
    words = text.split()
    while words and (words[0] in PREFIXES or words[0].startswith('{')):
        words = words[1:]
    if not words:
        return None
    m = words[0].lower()
    base = m[:-1] if m[-1:] in ('b', 'w', 'l', 'q') else m
    if m in OPTIONAL or base in OPTIONAL:
        return 'optional'
    if level == 1:
        if m in ABOVE_BASELINE or base in ABOVE_BASELINE or m.startswith('fisttp'):
            return 'above'
        if m == 'pextrw' and not text.rsplit(',', 1)[-1].strip().startswith('%'):
            return 'above'
    if level >= 3:   # x86-64-v3 has everything but AVX-512 (zmm and mask registers)
        return 'above' if AVX512_REG.search(text) else None
    if m in ABOVE_V2 or base in ABOVE_V2 or (m.startswith('v') and m not in NOT_VECTOR) or WIDE_REG.search(text):
        return 'above'
    return None


def dispatched(name, obj=None):
    for pattern, why in DISPATCHED:
        if pattern.search(name):
            return why
    for pattern, why in DISPATCHED_OBJECTS:
        if obj and pattern.search(obj):
            return why
    return None


def reviewed_dispatch(name, obj, mnemonics):
    """Narrow exceptions reviewed in the dependency sources and linked code; unexpected opcodes still fail.

    Zstandard huf_decompress.c HUF_decompress4X{1,2}_usingDTable_internal returns to the default decoder unless
    HUF_flags_bmi2 is set (ZSTD_cpuSupportsBmi2). libvpx inv_txfm_ssse3.c calls the two idct32 helpers only from
    vpx_idct32x32_{34,135}_add_ssse3, selected by vpx_dsp_rtcd. GCC random.cc random_device::_M_init tests CPUID
    leaf 1 ECX bit 30 and leaf 7 EBX bit 18 before selecting the RDRAND and RDSEED functions; the linked code was
    also checked, in MSYS2's libstdc++ (whose GCC mangles the helpers' internal linkage as _N_1L12...) and in the
    Steam Runtime sniper sysroot's GCC 10 one the Linux release links (_N_112..., without the L). Object and opcode
    restrictions prevent an unrelated function or future SIMD code being excused.
    """
    ops = {m if m == 'pmulhrsw' else m[:-1] if m[-1:] in ('b', 'w', 'l', 'q') else m for m in mnemonics}
    if not ops:
        return None
    if obj in ('huf_decompress.o', 'huf_decompress_amd64.o') and re.fullmatch(
            r'_?HUF_decompress4X[12]_usingDTable_internal_fast(?:_(?:c|asm)_loop)?', name):
        if ops <= {'shlx', 'shrx', 'andn', 'mulx'}:
            return 'zstd HUF_flags_bmi2 gate in HUF_decompress4X1/2_usingDTable_internal'
    if obj == 'inv_txfm_ssse3.c.o' and name in ('idct32_135_8x32_ssse3', 'idct32_34_8x32_ssse3'):
        if ops <= {'pmulhrsw'}:
            return 'libvpx vpx_idct32x32_34/135_add_ssse3 via vpx_dsp_rtcd'
    if obj == 'random.o' and re.fullmatch(r'_ZNSt12_GLOBAL__N_1L?(?:12__x86_(?:rdrand|rdseed)|19__x86_rdseed_rdrand)EPv',
                                          name):
        if ops <= {'rdrand', 'rdseed'}:
            return 'libstdc++ random_device::_M_init CPUID RDRAND/RDSEED checks'
    return None


def scan(objdump, path, level, link_map=None):
    """{(function, object or None): Counter of mnemonics} of the instructions outside the level, and the instruction
    count. With link_map each instruction is also attributed to the object file it came from."""
    proc = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', str(path)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, errors='replace', bufsize=1 << 20)
    found = collections.defaultdict(collections.Counter)
    func, total = '?', 0
    for line in proc.stdout:
        m = FUNC.match(line)
        if m:
            func = m.group(2)
            continue
        m = INSN.match(line)
        if not m:
            continue
        total += 1
        text = m.group(2)
        kind = classify(text, level)
        if kind:
            words = [w for w in text.split() if w not in PREFIXES and not w.startswith('{')]
            obj = link_map.object_at(int(m.group(1), 16)) if link_map else None
            symbol = link_map.symbol_at(int(m.group(1), 16)) if link_map else None
            found[(symbol or func, obj)][(kind, words[0] if words else '?')] += 1
    err = proc.stderr.read()
    code = proc.wait()
    proc.stdout.close()
    proc.stderr.close()
    if code:
        sys.exit(f'check_isa: {objdump} failed on {path}: {err.strip()[-400:]}')
    return found, total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--level', type=int, choices=(1, 2, 3), default=2,
                    help='x86-64 level to check against (1: baseline SSE2; default 2)')
    ap.add_argument('--objdump', help='objdump to use (default: llvm-objdump or objdump on PATH, else MSYS2 UCRT64)')
    ap.add_argument('--json', type=Path, help='also write the findings as JSON')
    ap.add_argument('--map', type=Path, help='lld map of the executable (default: same filename with .map extension)')
    args = ap.parse_args()
    objdump = find_objdump(args.objdump)
    if not objdump:
        sys.exit('check_isa: no objdump found' + (f' at {args.objdump}' if args.objdump else ''))
    report, bad = {}, 0
    for path in args.files:
        # Neighboring DLLs do not share the game's address-to-object mapping.
        map_path = args.map or path.with_suffix('.map')
        base = image_base(objdump, path) if map_path.is_file() else None
        link_map = LinkMap(map_path, base) if base is not None else None
        found, total = scan(objdump, path, args.level, link_map)
        unexpected, known, optional = {}, {}, {}
        for (fn, obj), counts in sorted(found.items(), key=lambda kv: (kv[0][0], kv[0][1] or '')):
            func = f'{fn} [{obj}]' if obj else fn
            above = {mn: n for (kind, mn), n in counts.items() if kind == 'above'}
            opt = {mn: n for (kind, mn), n in counts.items() if kind == 'optional'}
            why = dispatched(fn, obj)
            if above:
                above_why = why or reviewed_dispatch(fn, obj, above)
                (known if above_why else unexpected)[func] = {'mnemonics': above,
                                                            **({'check': above_why} if above_why else {})}
            if opt:
                opt_why = why or reviewed_dispatch(fn, obj, opt)
                optional[func] = {'mnemonics': opt, **({'check': opt_why} if opt_why else {})}
        report[str(path)] = {'instructions': total, 'level': args.level, 'unexpected': unexpected,
                             'dispatched': known, 'optional_extensions': optional}
        if link_map:
            print(f'{path}: objects from {map_path}')
        print(f'{path}: {total:,} instructions; above x86-64-v{args.level}: {len(unexpected)} functions unexpected, '
              f'{len(known)} behind a CPU check; extensions outside both levels in {len(optional)} functions')
        for func, d in list(unexpected.items())[:60]:
            top = ', '.join(f'{m} x{n}' for m, n in sorted(d['mnemonics'].items(), key=lambda x: -x[1])[:5])
            print(f'  UNEXPECTED {func}: {top}')
        if len(unexpected) > 60:
            print(f'  ... and {len(unexpected) - 60} more')
        by_check = collections.Counter(d['check'] for d in known.values())
        for why, n in by_check.most_common():
            print(f'  dispatched: {n} functions, {why}')
        for func, d in optional.items():
            print(f'  extension {func}: {", ".join(sorted(d["mnemonics"]))}' + (f' ({d["check"]})' if 'check' in d
                                                                                   else ' (NO KNOWN CHECK)'))
        bad += len(unexpected) + sum('check' not in d for d in optional.values())
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
