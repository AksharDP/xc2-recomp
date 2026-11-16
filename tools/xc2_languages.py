"""The game languages a set of XC2 game files has text for: the bdat/<folder> directories of bf2.arh.

XC2 turns the console language into the folder of its text (main 3.0.1: the table built at 0x18f228, indexed by
nn::settings::Language): ja jp, en-US gb, fr fr, de ge, it it, es sp, zh-CN cn, ko kr, nl nl, pt pt, ru ru, zh-TW tw,
en-GB gb, fr-CA fr, es-419 sp. At boot it stores that language without checking that the folder exists, so a language
whose folder is missing leaves the game on a black screen (GitHub issue 107). The releases differ: the Americas and
Europe base has fr gb ge it sp and update 3.0.1 adds kr; the Japanese base has jp cn tw. No release has nl, pt or ru.
The game knows no zh-Hans, zh-Hant or pt-BR (Switch codes newer than its SDK), so they are never offered.

Sources, each giving the folders of the game as it runs:
  from_romfs(dir)                      an extracted or installed RomFS (romfs.bin and index.json)
  from_game(base, keys, update=None)   the game files read in place: the base RomFS, or with an update the RomFS that
                                       update patches it to (tools/xc2_merge_romfs.py's BKTR plan, read piece by piece)

    python tools/xc2_languages.py --romfs DIR
    python tools/xc2_languages.py --base FILE [--update FILE] --keys FILE
print {"folders": [...], "languages": [...]} as JSON. Key values are read from the keys file and never printed.
"""
import argparse
import bisect
import json
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS / 'vendor' / 'macnativegames'))
sys.path.insert(0, str(TOOLS))
from xc2_prompts import Archive  # noqa: E402

# The Switch codes the game can show, in the order the installer and the menu list them, and each one's text folder.
FOLDER = {'en-US': 'gb', 'en-GB': 'gb', 'ja': 'jp', 'fr': 'fr', 'fr-CA': 'fr', 'de': 'ge', 'it': 'it', 'es': 'sp',
          'es-419': 'sp', 'ko': 'kr', 'zh-CN': 'cn', 'zh-TW': 'tw'}
CODES = tuple(FOLDER)
IVFC_DATA = 5   # the IVFC level that is the RomFS itself


def languages(folders):
    """The codes of CODES whose folder is among folders, in CODES order."""
    return [c for c in CODES if FOLDER[c] in set(folders)]


def arh_folders(arh):
    """The text folders named in bf2.arh (its bytes): /bdat/<folder>/<file>."""
    names = Archive.from_header(arh).names()
    return sorted({n.split('/')[2] for n in names if n.startswith('/bdat/') and n.count('/') == 3})


def romfs_file(read, name):
    """The bytes of a file in the root directory of a RomFS, read(offset, size) giving the RomFS's bytes."""
    h = struct.unpack('<10Q', read(0, 80))
    if h[0] != 80:
        raise ValueError('not a RomFS')
    dirs, files = read(h[3], h[4]), read(h[7], h[8])
    file = struct.unpack_from('<I', dirs, 12)[0]   # the root directory's first file
    while file != 0xffffffff:
        _, nxt, data, size, _, n = struct.unpack_from('<IIQQII', files, file)
        if files[file + 32:file + 32 + n].decode('utf-8', 'replace') == name:
            return read(h[9] + data, size)
        file = nxt
    raise ValueError(f'the RomFS has no {name}')


def from_romfs(romfs):
    """The text folders of an extracted RomFS folder (romfs.bin, index.json)."""
    index = json.loads((Path(romfs) / 'index.json').read_text())
    arh = next(f for f in index['files'] if f['path'] == 'bf2.arh')
    with open(Path(romfs) / 'romfs.bin', 'rb') as f:
        f.seek(arh['offset'])
        return arh_folders(f.read(arh['size']))


def from_game(base, keys_path, update=None):
    """The text folders of the game files, read in place: the base RomFS, or the RomFS update patches it to."""
    import xc2_merge_romfs as merge
    from extract_exefs import load_keys
    keys = load_keys(Path(keys_path))
    base_section = merge.Section(base, keys, False)
    handles = {}

    def handle(section):
        if section.path not in handles:
            handles[section.path] = section.path.open('rb')
        return handles[section.path]

    try:
        if update is None:
            first = base_section.level(IVFC_DATA)[0]

            def read(offset, size):
                return base_section.read(handle(base_section), first + offset, size)
        else:
            update_section = merge.Section(update, keys, True)
            pieces, _ = merge.plan(base_section, update_section, handle(update_section))
            starts, pos = [], 0
            for p in pieces:
                starts.append(pos)
                pos += p[2]

            def read(offset, size):
                out = bytearray()
                k = bisect.bisect_right(starts, offset) - 1
                while size > 0:
                    section, physical, length, generation = pieces[k]
                    skip = offset - starts[k]
                    n = min(size, length - skip)
                    out += section.read(handle(section), physical + skip, n, generation)
                    offset, size, k = offset + n, size - n, k + 1
                return bytes(out)
        return arh_folders(romfs_file(read, 'bf2.arh'))
    finally:
        del keys
        for f in handles.values():
            f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path)
    ap.add_argument('--base', type=Path)
    ap.add_argument('--update', type=Path)
    ap.add_argument('--keys', type=Path)
    args = ap.parse_args()
    if args.romfs:
        folders = from_romfs(args.romfs)
    elif args.base and args.keys:
        folders = from_game(args.base, args.keys, args.update)
    else:
        ap.error('give --romfs, or --base and --keys')
    print(json.dumps({'folders': folders, 'languages': languages(folders)}))


if __name__ == '__main__':
    main()
