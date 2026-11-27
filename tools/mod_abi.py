"""The key that ties a mod library (tools/mod_code.py) to the build of the game it loads into: everything both sides must
agree on. tools/build_game.py puts it into the exe (rt_mod_abi) and leaves what the key needs of the lift beside the exe,
in build-cache/mod-base:

  patches.txt     the patch table the game was lifted with (its sets have the switch numbers the exe uses)
  folded.txt      main's bytes the lifted code took as constants (recomp/gen.py), which data writes must stay off
  cpu-level.txt   the x86-64 level the exe was compiled for (build_game.py --cpu, runtime/cpu_level.h): 2 or 3

tools/mod_code.py works the key out again from that folder, the runtime headers and the generator it is about to use, and
stops when it differs from the exe's: a library made of other headers would not fit.

A library is compiled for the level of the exe it loads into (flags), and the level is in the key: a library for
x86-64-v3 holds AVX2, FMA and BMI2, which stop a processor without them, so a v2 exe must not load one, and a v3 exe
gets code made for its processor. A mod-base without cpu-level.txt comes from a build before --cpu v2 existed: level 3.

Standard library only.
"""
import hashlib
from pathlib import Path

VERSION = 1
HEADERS = ('cpu.h', 'simd.h', 'simd_fp.h', 'modcode_abi.h')   # what a mod library includes
# A mod library's code is generated code without the build's extras (hooks, ThinLTO, profiles); flags adds the -march of
# the build's level.
FLAGS = ('-O2', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden')
LEVELS = (2, 3)
LEVEL_FILE = 'cpu-level.txt'
# Runtime sources a library of a level compiles in. Without FMA3 clang turns simd.h's fused multiply-adds into calls of
# fma and fmaf. The exe takes those from runtime/fma.c (rounded once, FTZ and DAZ as the hardware's; its comment says
# why the C library's are no substitute), and a library must compute them the same way, so it holds its own copy.
SOURCES = {2: ('fma.c',), 3: ()}
ROOT = Path(__file__).resolve().parent.parent


def _text(path):
    """A file's text, line ends made one form: a release and a checkout may differ in them."""
    return path.read_bytes().replace(b'\r\n', b'\n')


def flags(level):
    """The compile flags of a mod library for an exe of x86-64 level 2 or 3."""
    return (*FLAGS, f'-march=x86-64-v{level}')


def level(base):
    """The x86-64 level of the exe a mod-base folder belongs to (cpu-level.txt); 3 when the folder records none."""
    path = base / LEVEL_FILE
    if not path.is_file():
        return 3
    text = path.read_text(encoding='utf-8').strip()
    if text not in [str(n) for n in LEVELS]:
        raise ValueError(f'{path}: "{text[:20]}" is not an x86-64 level ({" or ".join(map(str, LEVELS))})')
    return int(text)


def mod_abi(runtime, base, recomp=None):
    """runtime: the folder of the runtime headers; base: the mod-base folder; recomp: the generator's (default: the one
    beside this file). The flags carry the level. Level 3 is worked out as before levels existed, so the mod-base of
    an exe built then, which has no cpu-level.txt, still gives that exe's key."""
    recomp = recomp or ROOT / 'recomp'
    lv = level(base)
    h = hashlib.sha256(f'mod-abi {VERSION}\0{" ".join(flags(lv))}\0'.encode())
    files = [runtime / n for n in (*HEADERS, *SOURCES[lv])] + sorted(recomp.glob('*.py')) \
        + [base / 'patches.txt', base / 'folded.txt']
    for f in files:
        data = _text(f) if f.is_file() else b''
        h.update(f'{f.name}\0{len(data)}\0'.encode() + data)
    return h.hexdigest()[:32]
