"""The code of the player's mods, made at game start: the ExeFS patches and code cheats of the mods folder become a mod
library (runtime/modcode_abi.h), a DLL with only the functions they change, lifted again with the patches in. The game
(runtime/modcode.c) runs this when the mods folder holds something the last run did not see, waits for it, and loads the
library; xc2.exe itself never changes. One to three seconds, most of it the compiler.

  python tools/mod_code.py --exefs <dir> --mods <dir> --cache <build-cache> --runtime <dir> --clang <clang>
                           [--patches <patch file>] [--sysroot <dir>] [--abi <key>] [--progress]

--cache is the build-cache folder beside xc2.exe. Read there: mod-base/ (tools/mod_abi.py). Written there, all at the end
of a run that worked, so a failed one leaves the last good state:

  cheats.status, exefs.status, exefs.data    tools/cheats.py and tools/exefs_patches.py have the formats
  mods/<hash>.dll                            the library (.so on Linux); none when no mod changes code
  mods/current.txt                           the build's key, the library's name or "-", one per line

mods/obj keeps the compiled objects (the newest of earlier runs too), so an edit compiles again only the objects whose
functions changed.

--patches is the patch table the mods' sites go on top of (default: mod-base/patches.txt, the one the exe was built with).
A developer's edited table (recomp/patches-v301.txt) works too: a function whose sites differ from the built ones goes
into the library, so a change to the port's own patches runs after a restart instead of a build of the game. --abi is
the key in the exe; another key than this folder's stops the run (exit 3). --progress prints a line "progress <percent>
<what it does>" at each step, which the game draws as a bar in its console.

A patch the generator refuses costs its own file or cheat only: its status line becomes unsupported with the reason.

Needs recomp/ with numpy, and clang with lld. Game-derived output: everything here stays in ignored folders.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from argparse import Namespace
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path[:0] = [str(TOOLS), str(TOOLS.parent / 'recomp')]
from gen import Emitter, discover, fname  # noqa: E402  (first: it sets numpy's thread variables)
import cheats  # noqa: E402
import exefs_patches  # noqa: E402
import mod_abi  # noqa: E402
import numpy as np  # noqa: E402
import patches  # noqa: E402
import regs  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402
from layout import layout  # noqa: E402
from lifter import hx  # noqa: E402
from module import Module  # noqa: E402

WIN = os.name == 'nt'
PARTS = 32        # objects of a library; a function's object follows from its address, so an edit leaves the others alone
KEPT_OBJECTS = 64   # of earlier runs: switching between two values of an edit compiles each once
MAX_SETS = 4096   # runtime/patches.c PATCH_MAX
NO_WINDOW = 0x08000000 if WIN else 0   # CREATE_NO_WINDOW


class AbiError(Exception):
    pass


def run_tools(args, work):
    """tools/cheats.py and tools/exefs_patches.py over the mods folder, into work: the patch table with the mods' sets and
    the three files of the runtime."""
    table = args.patches or args.cache / 'mod-base' / 'patches.txt'
    paths = Namespace(exefs=args.exefs, mods=args.mods, status=work / 'cheats.status', out=work / 'cheats.txt',
                      patches=table)
    cheats.build(paths)
    exefs_patches.build(Namespace(exefs=args.exefs, mods=args.mods, patches=paths.out, out=work / 'patches.txt',
                                  status=work / 'exefs.status', data=work / 'exefs.data'))
    exefs_patches.recheck(Namespace(exefs=args.exefs, status=work / 'exefs.status', data=work / 'exefs.data',
                                    folded=args.cache / 'mod-base' / 'folded.txt'))
    return work / 'patches.txt'


def named_sites(table, module):
    """{offset: ((set name, replacement), ...)} in file order, and the spin waits: what the lifted code of a module takes
    from the table."""
    names = [n for n, _, _ in table.sets]
    sites = {off: tuple((names[k], tuple(repl)) for k, repl, _ in alts)
             for off, alts in table.sites.get(module, {}).items()}
    for back, head in table.spins.get(module, {}).items():
        sites[('spin', back)] = head
    return sites


def changed_sites(base, new, module):
    """The offsets whose sites (or spin waits) differ between two tables."""
    a, b = named_sites(base, module), named_sites(new, module)
    return sorted({k[1] if isinstance(k, tuple) else k for k in a.keys() | b.keys() if a.get(k) != b.get(k)})


def number_sets(base, new):
    """(switch number of each of new's sets, [the sets the build lacks]): the build's sets keep their numbers."""
    have = {n: i for i, (n, _, _) in enumerate(base.sets)}
    added = [s for s in new.sets if s[0] not in have]
    number = {**have, **{s[0]: len(base.sets) + i for i, s in enumerate(added)}}
    return [number[n] for n, _, _ in new.sets], added


class Part:
    """One object of the library: functions of any module, by address."""

    def __init__(self, number):
        self.number = number
        self.fns = []      # (address, C text, [set numbers])
        self.refs = set()

    def text(self):
        own = {a for a, _, _ in self.fns}
        imports = sorted(self.refs - own)
        out = [f'// {GENERATED_MARKER}: functions changed by mods, generated by tools/mod_code.py. Do not edit or commit.',
               '#define MOD_LIBRARY', '#include "cpu.h"', '#include "simd.h"', '#include "modcode_abi.h"', '',
               'extern ModRt mod_rt;',
               '#define rt_dispatch (mod_rt.dispatch)', '#define rt_spin_gate (mod_rt.spin_gate)',
               '#define rt_svc (mod_rt.svc)', '#define rt_brk (mod_rt.brk)', '#define rt_udf (mod_rt.udf)',
               '#define rt_ticks (mod_rt.ticks)', '#define rt_patch_on (mod_rt.patch_on)', '',
               f'static GuestFn mod_imports[{max(1, len(imports))}];']
        out += [f'#define {fname(a)} (mod_imports[{i}])' for i, a in enumerate(imports)]
        out += [f'void {fname(a)}(Cpu *c);' for a in sorted(own)]
        out += ['', *(text for _, text, _ in self.fns), '']
        for a, _, sets in self.fns:
            if sets:
                out.append(f'static const uint16_t sets_{a:x}[] = {{{", ".join(map(str, sets))}}};')
        out += ['static const ModFn fns[] = {']
        out += [f'    {{{hx(a)}, {fname(a)}, {f"sets_{a:x}" if sets else "0"}, {len(sets)}}},' for a, _, sets in self.fns]
        out += ['};', f'static const uint64_t import_addrs[{max(1, len(imports))}] = {{'
                + ', '.join(hx(a) for a in imports) + '};',
                f'extern const ModPart mod_part_{self.number};',
                f'const ModPart mod_part_{self.number} = {{fns, {len(self.fns)}, import_addrs, mod_imports, '
                f'{len(imports)}}};', '']
        return '\n'.join(out)


def c_string(s):
    return '"' + ''.join(ch if ' ' <= ch <= '~' and ch not in '"\\' else f'\\{ord(ch) & 255:03o}' for ch in s) + '"'


def main_text(key, first, added, parts):
    """The library's entry point: its sets and parts."""
    out = [f'// {GENERATED_MARKER}: a mod library\'s tables, generated by tools/mod_code.py. Do not edit or commit.',
           '#define MOD_LIBRARY', '#include "modcode_abi.h"', '', 'ModRt mod_rt;']
    out += [f'extern const ModPart mod_part_{p.number};' for p in parts]
    out += ['static const ModSet sets[] = {']
    out += [f'    {{{c_string(n)}, {c_string(e)}, {d}}},' for n, e, d in added]
    out += ['    {0, 0, 0}};', 'static const ModPart *const parts[] = {']
    out += [f'    &mod_part_{p.number},' for p in parts]
    export = '__declspec(dllexport)' if WIN else '__attribute__((visibility("default")))'
    out += ['    0};',
            f'static const ModLib lib = {{MOD_ABI_VERSION, {c_string(key)}, {first}, {len(added)}, sets, parts, '
            f'{len(parts)}, &mod_rt}};',
            f'{export} const ModLib *xc2_mod_lib(void);',
            f'{export} const ModLib *xc2_mod_lib(void) {{ return &lib; }}', '']
    return '\n'.join(out)


class Refused(Exception):
    """A site the generator refuses: the mod sets it belongs to, and why."""

    def __init__(self, sets, why):
        super().__init__(why)
        self.sets, self.why = sets, why


def lift(args, base, new, mods):
    """([Part], [the sets the build lacks], {module: [(start, end)] it read as constants}) of the functions whose sites
    differ between the tables."""
    numbers, added = number_sets(base, new)
    if len(base.sets) + len(added) > MAX_SETS:
        raise SystemExit(f'mod_code: {len(added)} patch sets are too many (runtime/patches.c PATCH_MAX)')
    names = [n for n, _, _ in new.sets]
    parts, reads = {}, {}
    for name, mbase, _ in mods:
        offsets = changed_sites(base, new, name)
        if not offsets:
            continue
        mod = Module(name, args.exefs / name, mbase, 'c')
        patches.check(new, mod, name)
        starts = mod.lo + discover(mod)[0] * 4
        ends = np.append(starts[1:], mod.hi)
        # the jump tables' reads, as recomp/gen.py _emitter records them
        read, seen = mod.read, reads.setdefault(name, [])

        def recorded(a, n, signed=False, read=read, seen=seen, mbase=mbase):
            seen.append((a - mbase, a - mbase + n))
            return read(a, n, signed=signed)
        mod.read = recorded
        patched = {a: [(numbers[k], words) for k, words in alts] for a, alts in new.for_module(name, mbase).items()}
        emitter = Emitter(mod, set(starts.tolist()), patched, reg_locals=regs.LOCALS, spins=new.spins_for(name, mbase))
        built = {off: {n for n, _ in alts} for off, alts in named_sites(base, name).items() if not isinstance(off, tuple)}
        for i in sorted({int(np.searchsorted(starts, mbase + off, 'right')) - 1 for off in offsets}):
            entry, end = int(starts[i]), int(ends[i])
            mine = [off for off in offsets if entry <= mbase + off < end]
            by_set = {n for off in mine for n in
                      {names[k] for k, _, _ in new.sites.get(name, {}).get(off, ())} - built.get(off, set())}
            mod_sets = {n for n in by_set if n.startswith(('cheat_', 'mod_'))}
            try:
                lines, refs = emitter.function(entry, end)
            except ValueError as e:   # patches.PatchError is one
                if not mod_sets:
                    raise
                m = re.match(r'(?:0x)?([0-9a-f]+):', str(e))   # the site, when the message names it
                off = int(m.group(1), 16) - mbase if m else None
                here = {names[k] for k, _, _ in new.sites.get(name, {}).get(off, ())} & mod_sets
                raise Refused(here or mod_sets, f'the generator refuses it ({e})') from None
            # a change of the port's own sites (a developer's table) runs always
            only_mods = all(not isinstance(off, tuple) and new_only_adds(base, new, name, off) for off in mine)
            sets = sorted(numbers[names.index(n)] for n in mod_sets) if only_mods else []
            number = int(hashlib.sha256(str(entry).encode()).hexdigest(), 16) % PARTS
            part = parts.setdefault(number, Part(number))
            part.fns.append((entry, '\n'.join(lines), sets))
            part.refs |= refs
    for p in parts.values():
        p.fns.sort()
    return [parts[k] for k in sorted(parts)], added, reads


def new_only_adds(base, new, module, off):
    """True when the site at off only gained alternatives of mods' sets: with them off it runs as built."""
    a, b = named_sites(base, module).get(off, ()), named_sites(new, module).get(off, ())
    if named_sites(base, module).get(('spin', off)) != named_sites(new, module).get(('spin', off)):
        return False
    return tuple(x for x in b if not x[0].startswith(('cheat_', 'mod_'))) == a


def refuse(work, sets, why):
    """Marks the files and cheats of refused sets unsupported in the status files."""
    for kind, status in (('mod_', 'exefs.status'), ('cheat_', 'cheats.status')):
        keys = {s[len(kind):].upper() for s in sets if s.startswith(kind)}
        if not keys:
            continue
        path = work / status
        lines = []
        for line in path.read_text(encoding='utf-8').splitlines():
            f = line.split('\t')
            if f[0] in keys and f[2] not in ('unsupported', 'other'):
                f = [f[0], f[1], 'unsupported', '-' if kind == 'mod_' else '0', why.replace('\t', ' ')[:200]]
                print(f'mod_code: {f[0]} is unsupported: {why}')
            lines.append('\t'.join(f))
        path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        if kind == 'mod_':
            data = work / 'exefs.data'
            kept = [x for x in data.read_text(encoding='utf-8').splitlines() if x.split('\t')[0] not in keys]
            data.write_text('\n'.join(kept) + '\n', encoding='utf-8')


def recheck_reads(args, work, reads):
    """A patched function can read bytes of main the build's lift did not: data writes over them are refused too."""
    folded = args.cache / 'mod-base' / 'folded.txt'
    have = exefs_patches.read_ranges(folded) if folded.is_file() else []
    new = sorted({r for r in reads if not any(s <= r[0] and r[1] <= e for s, e in have)})
    if new:
        (work / 'folded.txt').write_text(''.join(f'{s:x} {e:x}\n' for s, e in sorted(have + new)))
        exefs_patches.recheck(Namespace(exefs=args.exefs, status=work / 'exefs.status', data=work / 'exefs.data',
                                        folded=work / 'folded.txt'))


def without_sets(path, sets):
    """The patch file at path without the lines of some sets."""
    lines = [x for x in path.read_text().splitlines()
             if not ((f := x.split('#', 1)[0].split()) and f[0] in ('set', 'site') and f[1] in sets)]
    path.write_text('\n'.join(lines) + '\n')


def progress(args, percent, text):
    if args.progress:
        print(f'progress {percent:.0f} {text}', flush=True)


def run(cmd, env):
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, errors='replace', env=env,
                           creationflags=NO_WINDOW)
    except OSError as e:
        raise SystemExit(f'mod_code: {cmd[0]} does not start: {e}') from None
    if p.returncode:
        raise SystemExit(f'mod_code: {Path(str(cmd[0])).name} failed ({p.returncode}):\n{p.stderr or p.stdout}')


def compile_lib(args, key, parts, added, first):
    """Compiles the library into <cache>/mods; its file name. Objects are kept in mods/obj by the hash of their text."""
    folder = args.cache / 'mods'
    obj = folder / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    # tools/build_game.py compiles against the sysroot the same way
    sysroot = ['--target=x86_64-linux-gnu', f'--sysroot={args.sysroot}'] if args.sysroot else []
    flags = [*mod_abi.FLAGS, *([] if WIN else ['-fPIC']), *sysroot]
    units = [(f'part{p.number}', p.text()) for p in parts] + [('main', main_text(key, first, added, parts))]
    salt = f'{key}\0{" ".join(mod_abi.FLAGS)}\0'   # no folder: the AppImage's sysroot has another at each start
    hashes = [hashlib.sha256((salt + t).encode()).hexdigest()[:24] for _, t in units]
    lib = folder / (hashlib.sha256('\0'.join(hashes).encode()).hexdigest()[:16] + ('.dll' if WIN else '.so'))
    env = dict(os.environ)
    env['PATH'] = str(args.clang.parent) + os.pathsep + env.get('PATH', '')

    lock, done = threading.Lock(), 0

    def build(unit):
        (name, text), digest = unit
        o = obj / f'{digest}.o'
        if o.is_file():
            return o
        src, tmp = obj / f'{digest}.c', obj / f'{digest}.{os.getpid()}.tmp'
        src.write_text(text, encoding='utf-8', newline='\n')
        try:
            run([args.clang, *flags, '-I', args.runtime, '-c', src, '-o', tmp], env)
            os.replace(tmp, o)
        finally:
            src.unlink(missing_ok=True)
            tmp.unlink(missing_ok=True)
        return o

    def counted(unit):
        nonlocal done
        o = build(unit)
        with lock:
            done += 1
            progress(args, 35 + 55 * done / len(units), f'compiled {done} of {len(units)} parts')
        return o

    if not lib.is_file():
        progress(args, 35, f'compiling {len(units)} parts')
        with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            objects = list(ex.map(counted, zip(units, hashes)))
        progress(args, 92, 'linking')
        tmp = folder / f'{lib.stem}.{os.getpid()}.tmp'
        try:
            run([args.clang, '-shared', '-fuse-ld=lld', *sysroot, *objects, '-o', tmp], env)
            os.replace(tmp, lib)
        finally:
            tmp.unlink(missing_ok=True)
    old = sorted((f for f in obj.glob('*.o') if f.stem not in hashes), key=lambda f: f.stat().st_mtime)
    for f in old[:-KEPT_OBJECTS]:
        f.unlink(missing_ok=True)
    return lib


def publish(args, work, key, lib):
    """The run's files into the cache. current.txt goes first and comes back last: a run killed between would leave status
    files that describe another library, and without current.txt the game runs this again. Libraries no longer named go
    (one still loaded stays)."""
    folder = args.cache / 'mods'
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'current.txt').unlink(missing_ok=True)
    for name in ('cheats.status', 'exefs.status', 'exefs.data'):
        os.replace(work / name, args.cache / name)
    (work / 'current.txt').write_text(f'{key}\n{lib.name if lib else "-"}\n', encoding='utf-8', newline='\n')
    os.replace(work / 'current.txt', folder / 'current.txt')
    for f in folder.iterdir():
        if f.suffix in ('.dll', '.so') and (lib is None or f.name != lib.name):
            try:
                f.unlink()
            except OSError:
                pass


def build(args):
    t0 = time.perf_counter()
    key = mod_abi.mod_abi(args.runtime, args.cache / 'mod-base', TOOLS.parent / 'recomp')
    if args.abi and args.abi != key:
        raise AbiError(f'this folder\'s key is {key}, the game\'s is {args.abi}: the game was built by another setup')
    work = args.cache / 'mods' / f'work-{os.getpid()}'
    work.mkdir(parents=True, exist_ok=True)
    try:
        progress(args, 5, 'reading the mods and cheats')
        table = run_tools(args, work)
        progress(args, 20, "reading the game's code")
        mods = layout(args.exefs)
        base = patches.load(args.cache / 'mod-base' / 'patches.txt', mods)
        while True:
            new = patches.load(table, mods)
            try:
                parts, added, reads = lift(args, base, new, mods)
                break
            except Refused as e:
                refuse(work, e.sets, e.why)
                without_sets(table, e.sets)
        recheck_reads(args, work, reads.get('main', []))
        t1 = time.perf_counter()
        lib = compile_lib(args, key, parts, added, len(base.sets)) if parts else None
        publish(args, work, key, lib)
        progress(args, 100, 'done')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f'mod_code: {sum(len(p.fns) for p in parts)} functions, {len(added)} patch sets, '
          f'{lib.name if lib else "no library"}; {t1 - t0:.1f} s to lift, {time.perf_counter() - t1:.1f} s to compile')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, required=True, help='the mods folder')
    ap.add_argument('--cache', type=Path, required=True, help='build-cache beside xc2.exe')
    ap.add_argument('--runtime', type=Path, required=True, help='the runtime headers')
    ap.add_argument('--clang', type=Path, required=True)
    ap.add_argument('--patches', type=Path, help='patch table to use instead of the one the exe was built with')
    ap.add_argument('--sysroot', type=Path)
    ap.add_argument('--abi', help='the key in the exe (rt_mod_abi)')
    ap.add_argument('--progress', action='store_true', help='print a line at each step for a progress bar')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace', line_buffering=True)   # the game shows the lines as they come
    try:
        build(args)
    except AbiError as e:
        print(f'mod_code: {e}')
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
