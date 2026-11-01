"""Extract XCX DE: ExeFS of the base game and the update, and the update's merged RomFS.

    .venv\\Scripts\\python tools\\extract_xcx.py --base <base.nsp> --update <update.nsp> --keys <prod.keys>
        [--hactool <hactool.exe>] [--work build]

Writes build/extracted/upd/{exefs,romfs}. The RomFS merge is tools/extract.py merge_update_romfs (hactool merges the
base Program NCA with the update's BKTR section and checks all six IVFC levels). XCX has no reference hashes yet, so
nothing is compared against known dumps. Key values are read by the tools from the keys file and never printed.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--update', type=Path, required=True)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--hactool', type=Path, default=extract.ROOT / 'toolchain' / 'hactool' / 'hactool.exe')
    ap.add_argument('--work', type=Path, default=extract.ROOT / 'build')
    args = ap.parse_args()
    out = args.work.resolve() / 'extracted' / 'upd'
    logs = args.work.resolve() / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    if not (out / 'exefs' / 'main').is_file():
        extract.say('ExeFS of the update')
        extract.extract_exefs(args.update, args.keys, out, logs, update=True)
    if not (out / 'romfs' / 'index.json').is_file():
        extract.say('RomFS: base merged with the update')
        extract.merge_update_romfs(args.base, args.update, args.keys, args.hactool.resolve(), out / 'romfs', logs)
    extract.say(f'done: {out}')


if __name__ == '__main__':
    main()
