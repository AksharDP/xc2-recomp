"""Merge XC2's base and update Program NCAs into one RomFS image.

The vendored MacNativeGames merge reads the base NCA out of the original container with a macOS-only funopen range hook,
which the Windows hactool build does not have. This version copies both Program NCAs to a temporary directory and gives
hactool plain NCA paths, then verifies the BKTR result and indexes it exactly as the vendored extractor does.

Temporary space: the base NCA (about 13 GiB) plus the update NCA (about 170 MiB) plus the merged image.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
sys.path.insert(0, str(VENDOR))
sys.path.insert(0, str(ROOT / 'tools'))

import xc2_extract  # noqa: E402
from extract_exefs import (common_tickets, container_entries, load_keys, nca_inventory,  # noqa: E402
                           record_offset, require)
from extract_update_romfs import copy_range, hactool_prod_keys  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402


def merge(base, update, keys_path, hactool_path, output):
    require(not output.exists(), 'Output exists')
    keys = load_keys(keys_path)
    tickets = {}
    inputs = []
    for source, is_update in ((base, False), (update, True)):
        with source.open('rb') as f:
            kind, entries = container_entries(f, source.stat().st_size)
            require(not is_update or kind == 'nsp', 'Update must be an NSP')
            found = common_tickets(f, entries)
            for rights, key in found.items():
                require(rights not in tickets or tickets[rights] == key, 'Conflicting common tickets')
                tickets[rights] = key
            record, header = xc2_extract.program_nca(nca_inventory(f, entries, keys), update=is_update)
            rights = header[0x230:0x240]
            require(rights == bytes(16) or rights in tickets, 'Missing common ticket')
            inputs.append((source, record, header))
    sections = [i for i in range(4) if inputs[-1][2][0x400 + i * 512 + 4] == 4]
    require(len(sections) == 1, 'Expected one BKTR section')
    need = inputs[0][1]['size'] + inputs[1][1]['size'] + 20 * (1 << 30)
    require(shutil.disk_usage(output.parent).free > need, 'Need about 34 GiB free for the two NCAs and the image')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='xc2-merge-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        keyfile = tmp / 'title.keys'
        fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            for rights, key in tickets.items():
                f.write(f'{rights.hex()} = {key.hex()}\n')
        for (source, record, _), name in zip(inputs, ('base.nca', 'update.nca')):
            print(f'copying {source.name} Program NCA ({record["size"]:,} bytes) to {name}', flush=True)
            copy_range(source, record_offset(record), record['size'], tmp / name)
        result = tmp / 'result'
        result.mkdir()
        hactool_keys = tmp / 'hactool.keys'
        hactool_prod_keys(keys_path, hactool_keys)
        environment = dict(os.environ, MNG_TITLE_KEYS=str(keyfile))
        command = [str(hactool_path), '-x', '-y', '--disablekeywarns', '--suppresskeys',
                   '-k', str(hactool_keys), f'--basenca={tmp / "base.nca"}',
                   f'--section{sections[0]}={result / "romfs.bin"}', str(tmp / 'update.nca')]
        print('running hactool BKTR merge', flush=True)
        with (result / 'merge.log').open('w') as log:
            code = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT).returncode
        log_text = (result / 'merge.log').read_text()
        unsigned = re.compile(r'\s*(?:Fixed-Key Signature|NPDM Signature) \(FAIL\)')
        checked = '\n'.join(line for line in log_text.splitlines() if not unsigned.match(line))
        if code or re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', checked):
            (output.parent / (output.name + '-failure.log')).write_text(
                re.sub(r'[0-9A-Fa-f]{32,}', '<hex removed>', log_text))
        require(code == 0 and not re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', checked), 'BKTR verification failed')
        require(all(re.search(rf'Level {i} \(GOOD\)', log_text) for i in range(6)), 'Missing verified IVFC levels')
        image = result / 'romfs.bin'
        with image.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        print(f'indexing {image.stat().st_size:,} bytes', flush=True)
        files = index_romfs(image)
        report = dict(status='pass', romfs_size=image.stat().st_size, romfs_sha256=digest, files=files,
                      integrity='BKTR merge; all six IVFC levels verified')
        (result / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        result.rename(output)
    print(json.dumps(dict(status='pass', files=len(files), size=report['romfs_size'], sha256=digest)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('update', type=Path)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--hactool', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    merge(args.base.resolve(), args.update.resolve(), args.keys.resolve(), args.hactool.resolve(), args.output.resolve())


if __name__ == '__main__':
    main()
