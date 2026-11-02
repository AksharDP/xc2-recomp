"""Print the --probe-loader expectation table for a set of ExeFS modules, matching runtime/loader.c's counts.

    python tools/xc2_probe_expect.py build/extracted/v301/exefs
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402

MODULES = ('main', 'subsdk0', 'sdk')
STB_WEAK = 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exefs', type=Path)
    args = ap.parse_args()
    mods = [(n, Nso(args.exefs / n)) for n in MODULES]
    defined = {}
    for name, n in mods:
        defined[name] = {s for s, i, sh, v, sz in n.syms if sh}
    for idx, (name, n) in enumerate(mods):
        kinds = [0, 0, 0, 0]
        sym_import = sym_own = 0
        for off, kind, symidx, add in n.relocs:
            if kind == 1027:
                kinds[0] += 1
            else:
                k = {257: 1, 1025: 2, 1026: 3}.get(kind)
                if k is None:
                    raise SystemExit(f'{name}: relocation type {kind}')
                kinds[k] += 1
                if symidx < len(n.syms) and n.syms[symidx][2]:
                    sym_own += 1
                else:
                    sym_import += 1
        imports = weak = strong = 0
        to = [0] * len(mods)
        for sym, info, sh, v, sz in n.syms:
            if not sym or sh:
                continue
            imports += 1
            which = None
            for k, (oname, _) in enumerate(mods):
                if sym in defined[oname]:
                    which = k
                    break
            if which is not None:
                to[which] += 1
            elif (info >> 4) == STB_WEAK:
                weak += 1
            else:
                strong += 1
        init_off = n.dyn.get(12, 0)
        entries = n.dyn.get(27, 0) // 8
        total = sum(kinds)
        print(f'  // {name}: {imports} imports, {total} relocations, {strong} strong unresolved')
        print(f'  {{"{name}", {{{kinds[0]}, {kinds[1]}, {kinds[2]}, {kinds[3]}}}, {total}, {sym_import}, {sym_own}, '
              f'{imports}, {{{to[0]}, {to[1]}, {to[2]}}}, {weak}, {init_off:#x}, {entries}}},')


if __name__ == '__main__':
    main()
