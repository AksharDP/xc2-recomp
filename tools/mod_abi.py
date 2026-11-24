"""The key that ties a mod library (tools/mod_code.py) to the build of the game it loads into: everything both sides must
agree on. tools/build_game.py puts it into the exe (rt_mod_abi) and leaves what the key needs of the lift beside the exe,
in build-cache/mod-base:

  patches.txt   the patch table the game was lifted with (its sets have the switch numbers the exe uses)
  folded.txt    main's bytes the lifted code took as constants (recomp/gen.py), which data writes must stay off

tools/mod_code.py works the key out again from that folder, the runtime headers and the generator it is about to use, and
stops when it differs from the exe's: a library made of other headers would not fit.

Standard library only.
"""
import hashlib
from pathlib import Path

VERSION = 1
HEADERS = ('cpu.h', 'simd.h', 'simd_fp.h', 'modcode_abi.h')   # what a mod library includes
# A mod library's code is generated code without the build's extras (hooks, ThinLTO, profiles).
FLAGS = ('-O2', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
         '-march=x86-64-v3')
ROOT = Path(__file__).resolve().parent.parent


def _text(path):
    """A file's text, line ends made one form: a release and a checkout may differ in them."""
    return path.read_bytes().replace(b'\r\n', b'\n')


def mod_abi(runtime, base, recomp=None):
    """runtime: the folder of the runtime headers; base: the mod-base folder; recomp: the generator's (default: the one
    beside this file)."""
    recomp = recomp or ROOT / 'recomp'
    h = hashlib.sha256(f'mod-abi {VERSION}\0{" ".join(FLAGS)}\0'.encode())
    files = [runtime / n for n in HEADERS] + sorted(recomp.glob('*.py')) + [base / 'patches.txt', base / 'folded.txt']
    for f in files:
        data = _text(f) if f.is_file() else b''
        h.update(f'{f.name}\0{len(data)}\0'.encode() + data)
    return h.hexdigest()[:32]
