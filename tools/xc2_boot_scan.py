"""Find XC2's boot parameters: nn::init::Start's callback shape, and the return addresses of the first
SetFocusHandlingMode and nvnBootstrapLoader calls inside nnMain.

Disassembles the NSO .text with capstone: exports come from .dynsym, calls to imports are found by locating the PLT
stub that loads an import's GOT slot and then scanning for BL instructions that target the stub.

    python tools/xc2_boot_scan.py build/extracted/v301/exefs
    python tools/xc2_boot_scan.py build/extracted/v301/exefs --symbol _ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE
"""
import argparse
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402

MODS = ('main', 'subsdk0', 'sdk')


def load(exefs):
    out = {}
    for name in MODS:
        path = exefs / name
        if path.is_file():
            out[name] = Nso(path)
    return out


def plt_slots(n):
    """slot -> stub address, for the adrp/ldr/add/br stubs at the head of .text."""
    found = []
    text, size = n.text
    end = text + size
    img = n.image
    for a in range(text, end - 16, 4):
        w0, w1, w3 = struct.unpack_from('<III', img, a)[0], struct.unpack_from('<I', img, a + 4)[0], \
            struct.unpack_from('<I', img, a + 12)[0]
        if (w0 & 0x9F00001F) != 0x90000010 or (w1 & 0xFFC003FF) != 0xF9400211 or w3 != 0xD61F0220:
            continue
        imm = ((w0 >> 29) & 3) | (((w0 >> 5) & 0x7FFFF) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        slot = (a & ~0xFFF) + (imm << 12) + ((w1 >> 10) & 0xFFF) * 8
        found.append((slot, a))
    return found


def import_stub(n, name):
    """The PLT stub address for the import called `name` in module n, or None."""
    want = None
    for off, kind, symidx, add in n.relocs:
        if kind not in (1025, 1026):
            continue
        if symidx < len(n.syms) and n.syms[symidx][0] == name:
            want = off
            break
    if want is None:
        return None
    for slot, stub in plt_slots(n):
        if slot == want:
            return stub
    return None


def bl_targets(n, target):
    """Addresses of BL instructions in .text whose target is `target`."""
    out = []
    text, size = n.text
    img = n.image
    for a in range(text, text + size - 4, 4):
        w = struct.unpack_from('<I', img, a)[0]
        if w >> 26 != 0b100101:
            continue
        imm = w & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
        if a + imm * 4 == target:
            out.append(a)
    return out


def export(n, name):
    for sym, info, shndx, value, size in n.syms:
        if sym == name and shndx and value:
            return value, size
    return None


def dump(n, start, count, stop_at_ret=True):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = False
    text, size = n.text
    off = start
    for ins in md.disasm(bytes(n.image[off:off + count * 4]), off):
        print(f'  {ins.address:#0x}  {ins.mnemonic:8s} {ins.op_str}')
        if stop_at_ret and ins.mnemonic == 'ret':
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exefs', type=Path)
    ap.add_argument('--symbol', action='append', default=[])
    ap.add_argument('--calls-to', action='append', default=[])
    ap.add_argument('--start', help='module:offset to disassemble, e.g. sdk:0x1f0')
    ap.add_argument('--count', type=int, default=200)
    args = ap.parse_args()

    mods = load(args.exefs)
    for name, n in mods.items():
        print(f'{name}: text {n.text[0]:#x}+{n.text[1]:#x} exports {sum(1 for s in n.syms if s[2] and s[3])}')
    print()
    for name, n in mods.items():
        for sym in args.symbol:
            e = export(n, sym)
            if e:
                print(f'export {sym} in {name} at +{e[0]:#x} size {e[1]:#x}')
    print()
    for name, n in mods.items():
        for sym in args.calls_to:
            stub = import_stub(n, sym)
            if stub is None:
                continue
            calls = bl_targets(n, stub)
            print(f'{name}: import {sym} stub +{stub:#x}, {len(calls)} BL site(s)')
            for c in calls:
                print(f'   call at +{c:#x} returns to +{c + 4:#x}')
    if args.start:
        mname, _, off = args.start.partition(':')
        print(f'\ndisasm {mname}+{int(off, 0):#x}')
        dump(mods[mname], int(off, 0), args.count)


if __name__ == '__main__':
    main()
