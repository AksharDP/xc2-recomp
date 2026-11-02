"""Survey XC2's ExeFS modules against the host boundary of the runtime we forked.

Prints segment sizes, SDK markers, import/export counts, which undefined symbols the loaded modules resolve among
themselves, and which would need a host override. Also lists the HOST_OVERRIDE names the runtime already provides.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402

MODULES = ('main', 'subsdk0', 'sdk')


def overrides():
    names = set()
    for path in (ROOT / 'runtime').rglob('*.[ch]'):
        text = path.read_text(encoding='utf-8', errors='replace')
        names.update(re.findall(r'HOST_OVERRIDE\(\s*"([^"]+)"', text))
        names.update(re.findall(r'hostfn_register\(\s*"([^"]+)"', text))
    return names


def sdk_marker(module):
    image = bytes(module.image)
    found = set()
    for pat in (rb'NintendoSDK [0-9.]+', rb'NVN [0-9.]+', rb'nn::[a-z]+'):
        for m in re.finditer(pat, image):
            found.add(m.group(0).decode('utf-8', 'replace'))
    return sorted(found)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('exefs', type=Path)
    args = parser.parse_args()
    mods = {}
    for name in MODULES:
        path = args.exefs / name
        if not path.exists():
            continue
        n = Nso(path)
        mods[name] = n
        print(f'{name}: image 0x{n.size:x}  text 0x{n.text[0]:x}+0x{n.text[1]:x}  '
              f'ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  data 0x{n.data[0]:x}+0x{n.data[1]:x}  '
              f'bss 0x{n.bss_size:x}  build {n.build_id[:16]}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        markers = sdk_marker(n)
        print(f'  markers: {", ".join(markers[:12])}{" ..." if len(markers) > 12 else ""}')

    defined = {}
    for name, n in mods.items():
        for sym, info, shndx, value, size in n.syms:
            if sym and shndx:
                defined.setdefault(sym, []).append(name)
    undef = {}
    for name, n in mods.items():
        for sym, info, shndx, value, size in n.syms:
            if sym and not shndx:
                undef.setdefault(sym, set()).add(name)
    host = overrides()
    unresolved = sorted(s for s in undef if s not in defined and s not in host)
    resolved = sorted(s for s in undef if s in defined)
    have = sorted(s for s in undef if s not in defined and s in host)
    print(f'\nundefined symbols: {len(undef)}; defined in another module: {len(resolved)}; host override: {len(have)}; '
          f'unresolved: {len(unresolved)}')
    for tag, names in (('resolved by a module', resolved), ('host override', have), ('UNRESOLVED', unresolved)):
        print(f'\n-- {tag} ({len(names)})')
        for s in names:
            print('  ', s)
    if unresolved:
        Path(ROOT / 'build' / 'unresolved-symbols.txt').write_text('\n'.join(unresolved) + '\n')


if __name__ == '__main__':
    main()
