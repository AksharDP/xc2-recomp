"""Name module offsets and disassemble around them: xcx_sym.py sdk+0x13e81c main+0x13aa5ec [--dis N].

Reads the extracted ExeFS (build/extracted/upd/exefs). PLT stubs and GOT-loaded calls are named by their relocation."""
import bisect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
import capstone  # noqa: E402
from nso import Nso  # noqa: E402

EXEFS = ROOT / 'build' / 'extracted' / 'upd' / 'exefs'
mods = {}


def mod(name):
    if name not in mods:
        n = Nso(EXEFS / name)
        funcs = sorted((s[3], s[0]) for s in n.syms if s[3] and s[2])
        slots = {off: n.syms[sym][0] for off, typ, sym, _ in n.relocs if sym and typ in (1025, 1026, 257)}
        mods[name] = (n, [a for a, _ in funcs], [s for _, s in funcs], slots)
    return mods[name]


def name_of(m, off):
    n, addrs, names, _ = mod(m)
    i = bisect.bisect_right(addrs, off) - 1
    return f'{names[i]}+{off - addrs[i]:#x}' if i >= 0 else '?'


def plt_target(m, off):
    n, _, _, slots = mod(m)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    ins = list(md.disasm(bytes(n.image[off:off + 12]), off))
    if len(ins) >= 2 and ins[0].mnemonic == 'adrp' and ins[1].mnemonic == 'ldr':
        page = int(ins[0].op_str.split('#')[1], 16)
        imm = int(ins[1].op_str.split('#')[-1].rstrip(']'), 16)
        return slots.get(page + imm)
    return None


def dis(m, off, count):
    n = mod(m)[0]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    start = off - 4 * count
    for i in md.disasm(bytes(n.image[start:off + 4 * 4]), start):
        note = ''
        if i.mnemonic == 'bl':
            t = int(i.op_str.lstrip('#'), 16)
            note = plt_target(m, t) or name_of(m, t)
        mark = '>' if i.address == off else ' '
        print(f'  {mark} {m}+{i.address:#x}: {i.mnemonic:6} {i.op_str:30} {note}')


if __name__ == '__main__':
    args = sys.argv[1:]
    count = 0
    if '--dis' in args:
        k = args.index('--dis')
        count = int(args[k + 1])
        del args[k:k + 2]
    for a in args:
        m, off = a.split('+')
        off = int(off, 16)
        print(f'{a}: {name_of(m, off)}')
        if count:
            dis(m, off, count)
