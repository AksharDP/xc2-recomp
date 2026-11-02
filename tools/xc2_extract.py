"""Extraction front end for Xenoblade Chronicles 2.

The vendored MacNativeGames extractors (MIT) know Xenoblade Chronicles X's title ids. The NCA and container parsing is
game independent, so this module replaces only their program-NCA selection with XC2's ids and keeps their verified
readers. Base title 0100E95004038000, update 0100E95004038800.

Run with the .venv Python (3.12): cryptography is the only native dependency of these paths.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
sys.path.insert(0, str(VENDOR))

import extract_exefs  # noqa: E402
import extract_romfs  # noqa: E402
import extract_update_romfs  # noqa: E402
from extract_exefs import require  # noqa: E402

BASE_TITLE = '0100E95004038000'
UPDATE_TITLE = '0100E95004038800'

# Add-on content title ids and the index nn::aoc reports for each. The low three hex digits of a DLC title id are
# its AddOnContentIndex (0x063 = 99, the Japanese voice pack, as on the console).
DLC_TITLES = {
    '0100E95004039001': 1,    # Torna ~ The Golden Country
    '0100E95004039002': 2,    # New Quests
    '0100E95004039003': 3,    # New Rare Blade
    '0100E95004039004': 4,    # New Challenge Mode
    '0100E95004039005': 5,    # Helpful Items
    '0100E95004039006': 6,    # My Nintendo Bonus
    '0100E95004039063': 99,   # Japanese Voice
}


def is_bktr(hdr):
    return any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4))


def program_nca(records, update=False):
    """The one Program NCA of an XC2 base or update package.

    An update's Program NCA keeps the base title id; its Meta NCA carries the update id. DLC packages have no
    Program NCA and are rejected here (their PublicData RomFS is mounted separately).
    """
    if update:
        programs = [(r, h) for r, h in records if r['content_type'] == 0]
        require(len(programs) == 1, 'Expected one update Program NCA; combined packages are unsupported')
        record, hdr = programs[0]
        require(record['title_id'] in (BASE_TITLE, UPDATE_TITLE), 'Update belongs to another game')
        require(is_bktr(hdr), 'Expected a BKTR update, not a base game')
        require(all(r['title_id'] in (BASE_TITLE, UPDATE_TITLE) for r, _ in records), 'Mixed-title update package')
        return record, hdr
    metadata = [r for r, _ in records if r['content_type'] == 1]
    require(len(metadata) <= 1 and all(r['title_id'] == BASE_TITLE for r in metadata),
            'Expected base game metadata; updates, DLC and combined packages are unsupported')
    programs = [(r, h) for r, h in records if r['content_type'] == 0 and not is_bktr(h)]
    require(len(programs) == 1, 'Expected one base Program NCA')
    record, hdr = programs[0]
    require(record['title_id'] == BASE_TITLE, 'Expected Xenoblade Chronicles 2 base game')
    return record, hdr


def patch():
    """Point every vendored extractor at the XC2 selector (they import it by module global)."""
    extract_exefs.program_nca = program_nca
    extract_romfs.program_nca = program_nca
    extract_update_romfs.program_nca = program_nca


def exefs(game, keys, output, update):
    patch()
    output = output.resolve()
    require(not output.exists(), 'Output already exists; remove it or choose another directory')
    sys.argv = ['extract_exefs.py', str(game), '--keys', str(keys), '--output', str(output)]
    if update:
        sys.argv.append('--update')
    extract_exefs.main()


def base_romfs(keys, manifest, output):
    patch()
    sys.argv = ['extract_romfs.py', '--keys', str(keys), '--manifest', str(manifest), '--output', str(output.resolve())]
    extract_romfs.main()


def update_romfs(base, update, keys, hactool, output):
    patch()
    extract_update_romfs.merge(base.resolve(), update.resolve(), keys.resolve(), hactool.resolve(), output.resolve())


def unpack(image_path, output, files):
    """Write each indexed RomFS file under output, streaming from the image."""
    with image_path.open('rb') as src:
        for item in files:
            dest = output / item['path']
            dest.parent.mkdir(parents=True, exist_ok=True)
            src.seek(item['offset'])
            left = item['size']
            with dest.open('wb') as dst:
                while left:
                    chunk = src.read(min(1 << 20, left))
                    require(chunk, 'Truncated RomFS image')
                    dst.write(chunk)
                    left -= len(chunk)


def dlc_romfs(game, keys, output):
    """Extract one DLC NSP's PublicData RomFS as loose files under its add-on content directory.

    The runtime mounts <romfs>/../aoc/<index> as a read-only host directory (fs_host.c mount_aoc), so an add-on's
    files must sit directly in that directory rather than inside a romfs.bin image like the base game.
    """
    patch()
    game, output = game.resolve(), output.resolve()
    require(not output.exists(), 'Output already exists; remove it or choose another directory')
    from extract_exefs import (common_tickets, container_entries, load_keys, nca_inventory,  # noqa: E402
                               record_offset, require as check)
    keyring = load_keys(keys.resolve())
    with game.open('rb') as f:
        _, entries = container_entries(f, game.stat().st_size)
        tickets = common_tickets(f, entries)
        public = [(r, h) for r, h in nca_inventory(f, entries, keyring) if r['content_type'] == 5]
        check(len(public) == 1, 'Expected exactly one PublicData NCA')
        record, hdr = public[0]
        check(record['title_id'] in DLC_TITLES, f'Unexpected DLC title id {record["title_id"]}')
        index = DLC_TITLES[record['title_id']]
        manifest = {'source': str(game), 'source_size': game.stat().st_size, 'program': record}
    manifest_path = output.parent / f'dlc-{index}-manifest.json'
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest))
    # The vendored extractor selects a Program NCA; the PublicData NCA carries the same IVFC RomFS payload and the
    # selector only returns the record the manifest was built from. Restore the base selector afterwards.
    extract_romfs.program_nca = lambda records, update=False: public[0]
    try:
        sys.argv = ['extract_romfs.py', '--keys', str(keys), '--manifest', str(manifest_path),
                    '--output', str(output)]
        extract_romfs.main()
    finally:
        extract_romfs.program_nca = program_nca
        manifest_path.unlink(missing_ok=True)
    report = json.loads((output / 'index.json').read_text())
    image = output / 'romfs.bin'
    require(report.get('status') == 'pass' and image.exists(), 'DLC RomFS did not verify')
    files = report['files']
    unpack(image, output, files)
    image.unlink()
    report['title_id'] = record['title_id']
    report['add_on_content_index'] = index
    report['layout'] = 'loose files (aoc mount directory)'
    (output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': 'pass', 'index': index, 'title_id': record['title_id'],
                      'files': len(files), 'bytes': sum(item['size'] for item in files),
                      'sha256': report['romfs_sha256']}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('exefs', help='extract the Program ExeFS of a base or update NSP')
    p.add_argument('game', type=Path)
    p.add_argument('--keys', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--update', action='store_true')
    p = sub.add_parser('base-romfs', help='verify and extract the base RomFS from its ExeFS manifest')
    p.add_argument('--keys', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('update-romfs', help='merge the base NSP and the update BKTR section into one RomFS')
    p.add_argument('base', type=Path)
    p.add_argument('update', type=Path)
    p.add_argument('--keys', type=Path, required=True)
    p.add_argument('--hactool', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('dlc-romfs', help='verify and unpack one DLC NSP into its aoc/<index> directory')
    p.add_argument('game', type=Path)
    p.add_argument('--keys', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'exefs':
        exefs(args.game, args.keys, args.output, args.update)
    elif args.command == 'base-romfs':
        base_romfs(args.keys, args.manifest, args.output)
    elif args.command == 'dlc-romfs':
        dlc_romfs(args.game, args.keys, args.output)
    else:
        update_romfs(args.base, args.update, args.keys, args.hactool, args.output)


if __name__ == '__main__':
    main()
