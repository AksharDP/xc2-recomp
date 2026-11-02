"""Inspecting a Xenoblade Chronicles 2 NSP without extracting it.

Lists the container entries and every NCA's content type, title id, size and FS sections. Derived from MacNativeGames'
extract_exefs.py (MIT) so the parsing matches the extractor used later.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'macnativegames'))

from extract_exefs import (container_entries, load_keys, nca_inventory, read_at)  # noqa: E402

CONTENT = {0: 'Program', 1: 'Meta', 2: 'Control', 3: 'Manual', 4: 'Data', 5: 'PublicData'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--keys', type=Path, required=True)
    args = parser.parse_args()
    keys = load_keys(args.keys)
    size = args.game.stat().st_size
    with args.game.open('rb') as f:
        kind, entries = container_entries(f, size)
        print(f'{args.game.name}: {kind}, {size:,} bytes, {len(entries)} entries')
        for name, offset, length in entries:
            print(f'  entry {name:40s} {length:>14,}')
        records = nca_inventory(f, entries, keys)
        print(f'  {len(records)} NCAs')
        for record, hdr in records:
            sections = []
            for i in range(4):
                start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
                if start:
                    sections.append(f'{i}:{start * 512:#x}+{(end - start) * 512:#x} fs={hdr[0x400 + 512 * i + 2:0x400 + 512 * i + 5].hex()}')
            print(f'  {record["name"]} type={record["content_type"]}({CONTENT.get(record["content_type"], "?")}) '
                  f'title={record["title_id"]} size={record["size"]:,} sections=[{", ".join(sections)}]')


if __name__ == '__main__':
    main()
