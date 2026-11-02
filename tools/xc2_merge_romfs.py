"""Merge XC2's base game and update into one RomFS image, read in place from the NSP or XCI files.

An update's Program NCA holds a BKTR (patch) RomFS section: an indirect table maps every range of the patched RomFS to
either the base Program NCA's RomFS section or the update's own data, and an AES-CTR-EX table gives the counter
generation of each range of that data. This module walks both tables and writes the patched RomFS level (the image the
runtime reads) straight from the two game files: nothing is copied out first, so the only space it takes is the image.

Reading, decrypting and writing are pipelined: worker threads read and decrypt pieces of up to PIECE bytes (AES-CTR in
cryptography releases the GIL), and one writer puts them in order into the image and hashes them. The caller compares
the SHA-256 with the expected one (tools/data/expected-xc2-<version>.json), which checks every byte of the result.

Formats follow the public descriptions of the NCA BKTR section (switchbrew "NCA", hactool bktr.h): the bucket trees
are 0x4000-byte nodes, an L1 node listing the start offset of each entry set, then the entry sets.
"""
import argparse
import bisect
import hashlib
import json
import os
import shutil
import struct
import sys
import threading
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
sys.path.insert(0, str(VENDOR))
sys.path.insert(0, str(ROOT / 'tools'))

from package_select import program_nca  # noqa: E402
from extract_exefs import (common_tickets, container_entries, decrypt, load_keys, nca_ctr_key,  # noqa: E402
                           nca_inventory, record_offset, require)
from extract_romfs import index_romfs  # noqa: E402

NODE = 0x4000          # bucket tree node size
PIECE = 16 << 20       # the most one worker reads and decrypts at a time
WORKERS = max(2, min(8, os.cpu_count() or 2))
BKTR = 4               # NCA FS header encryption type of a patch section (AES-CTR-EX)
IVFC_DATA = 5          # the IVFC level that is the RomFS itself


class ShortRead(ValueError):
    """A game file ends inside the data the merge needs. path: that file."""

    def __init__(self, path):
        self.path = Path(path)
        super().__init__(f'{self.path} ends early')


class Section:
    """The RomFS (base) or patch (update) section of a Program NCA inside a game file."""

    def __init__(self, path, keys, update):
        self.path = Path(path)
        with self.path.open('rb') as f:
            _, entries = container_entries(f, self.path.stat().st_size)
            tickets = common_tickets(f, entries)
            record, hdr = program_nca(nca_inventory(f, entries, keys), update=update)
            self.key = nca_ctr_key(hdr, keys, tickets)
        self.nca = record_offset(record)
        wanted = BKTR if update else 3   # the base RomFS section is plain AES-CTR
        found = [i for i in range(4) if struct.unpack_from('<I', hdr, 0x240 + i * 16)[0] and
                 hdr[0x400 + i * 512 + 2:0x400 + i * 512 + 5] == bytes([0, 3, wanted])]
        require(len(found) == 1, f'{self.path.name}: expected one {"patch" if update else "RomFS"} section')
        i = found[0]
        self.fs = hdr[0x400 + i * 512:0x600 + i * 512]
        require(hashlib.sha256(self.fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
        start, end = struct.unpack_from('<II', hdr, 0x240 + i * 16)
        self.start, self.size = start * 512, (end - start) * 512   # within the NCA
        self.upper = self.fs[0x140:0x148][::-1]                     # counter bytes 0-7, from the FS header
        require(self.fs[8:12] == b'IVFC', f'{self.path.name}: expected an IVFC RomFS section')

    def level(self, n):
        offset, size, _, _ = struct.unpack_from('<QQII', self.fs, 0x18 + n * 24)
        return offset, size

    def iv(self, absolute, generation=None):
        """Counter for the 16-byte block at absolute (an offset within the NCA). A patch range's generation replaces
        counter bytes 4-7 (AES-CTR-EX)."""
        upper = self.upper if generation is None else self.upper[:4] + generation.to_bytes(4, 'big')
        return upper + (absolute // 16).to_bytes(8, 'big')

    def read(self, f, relative, size, generation=None):
        """Decrypted bytes [relative, relative + size) of the section."""
        require(0 <= relative and relative + size <= self.size, f'{self.path.name}: read outside its section')
        absolute = self.start + relative
        aligned = absolute & ~15
        f.seek(self.nca + aligned)
        data = f.read(size + absolute - aligned)
        if len(data) != size + absolute - aligned:
            raise ShortRead(self.path)
        out = Cipher(algorithms.AES(self.key), modes.CTR(self.iv(aligned, generation))).decryptor().update(data)
        return out[absolute - aligned:]


def bucket_tree(section, f, offset, size, count, entry_format):
    """Entries of a one-level bucket tree stored at [offset, offset + size) of the section, and the end offset of the
    range they cover. The tables are encrypted with the section's own counter."""
    raw = section.read(f, offset, size)
    _, sets, end = struct.unpack_from('<iiq', raw, 0)
    require(0 < sets and (1 + sets) * NODE <= size, 'BKTR bucket tree out of bounds')
    entry_size = struct.calcsize(entry_format)
    entries = []
    for s in range(sets):
        node = raw[(1 + s) * NODE:(2 + s) * NODE]
        _, n, _ = struct.unpack_from('<iiq', node, 0)
        require(0 < n <= (NODE - 16) // entry_size, 'BKTR entry set out of bounds')
        entries += [struct.unpack_from(entry_format, node, 16 + k * entry_size) for k in range(n)]
    require(len(entries) == count, 'BKTR entry count differs from its header')
    require(all(a[0] < b[0] for a, b in zip(entries, entries[1:])), 'BKTR entries out of order')
    return entries, end


def plan(base, update, f):
    """[(source, relative offset, size, generation)] pieces that, in order, make up the patched RomFS level."""
    fs = update.fs
    ind_offset, ind_size = struct.unpack_from('<QQ', fs, 0x100)
    ctr_offset, ctr_size = struct.unpack_from('<QQ', fs, 0x120)
    require(fs[0x110:0x114] == b'BKTR' and fs[0x130:0x134] == b'BKTR', 'Update section has no BKTR tables')
    indirect, virtual_end = bucket_tree(update, f, ind_offset, ind_size, struct.unpack_from('<I', fs, 0x118)[0], '<qqi')
    ctr_ex, _ = bucket_tree(update, f, ctr_offset, ctr_size, struct.unpack_from('<I', fs, 0x138)[0], '<qII')
    ctr_starts = [e[0] for e in ctr_ex]
    first, length = update.level(IVFC_DATA)
    require(first + length <= virtual_end, 'Update RomFS beyond its indirect table')

    def generations(physical, size):
        """Split a range of the update's data at its AES-CTR-EX boundaries: [(offset, size, generation)]."""
        k = bisect.bisect_right(ctr_starts, physical) - 1
        require(k >= 0, 'Update data before its first AES-CTR-EX entry')
        out = []
        while size:
            stop = ctr_starts[k + 1] if k + 1 < len(ctr_starts) else update.size
            n = min(size, stop - physical)
            require(n > 0, 'Update data beyond its AES-CTR-EX table')
            out.append((physical, n, ctr_ex[k][2]))
            physical, size, k = physical + n, size - n, k + 1
        return out

    pieces = []
    for k, (virtual, physical, storage) in enumerate(indirect):
        stop = indirect[k + 1][0] if k + 1 < len(indirect) else virtual_end
        lo, hi = max(virtual, first), min(stop, first + length)
        if lo >= hi:
            continue
        physical += lo - virtual
        require(storage in (0, 1), 'Unknown BKTR storage index')
        ranges = [(physical, hi - lo, None)] if storage == 0 else generations(physical, hi - lo)
        for offset, size, generation in ranges:
            for pos in range(0, size, PIECE):
                pieces.append((base if storage == 0 else update, offset + pos, min(PIECE, size - pos), generation))
    require(sum(p[2] for p in pieces) == length, 'BKTR tables do not cover the whole RomFS')
    return pieces, length


def merge(base_path, update_path, keys_path, output, progress=None):
    """Write output/romfs.bin and output/index.json. output must not exist; it is filled under a sibling .part name
    and renamed when complete, so an interrupted merge never leaves a folder that looks finished. progress(done, total)
    is called as pieces are written. Returns the index.json report."""
    output = Path(output)
    require(not output.exists(), 'Output exists')
    keys = load_keys(Path(keys_path))
    base, update = Section(base_path, keys, False), Section(update_path, keys, True)
    with update.path.open('rb') as f:
        pieces, total = plan(base, update, f)
    part = output.with_name(f'.{output.name}.part')
    if part.exists():
        shutil.rmtree(part)
    part.mkdir(parents=True)   # a plain mkdir: the folder inherits the install folder's permissions
    image = part / 'romfs.bin'
    local = threading.local()

    def work(piece):
        section, offset, size, generation = piece
        handles = getattr(local, 'handles', None)
        if handles is None:
            handles = local.handles = {}
        f = handles.get(section.path)
        if f is None:
            f = handles[section.path] = section.path.open('rb')
        return section.read(f, offset, size, generation)

    digest, done = hashlib.sha256(), 0
    with image.open('wb') as out, ThreadPoolExecutor(WORKERS) as pool:
        out.truncate(total)   # one allocation up front: less fragmentation, and a full disk fails here, not at 90%
        out.seek(0)
        queue, todo = deque(), iter(pieces)
        for piece in todo:
            queue.append(pool.submit(work, piece))
            if len(queue) >= 2 * WORKERS:
                break
        while queue:
            data = queue.popleft().result()
            nxt = next(todo, None)
            if nxt is not None:
                queue.append(pool.submit(work, nxt))
            out.write(data)
            digest.update(data)
            done += len(data)
            if progress:
                progress(done, total)
    files = index_romfs(image)
    report = dict(status='pass', romfs_size=total, romfs_sha256=digest.hexdigest(), files=files,
                  integrity='BKTR merge read in place; the caller checks the SHA-256 against the expected one')
    (part / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    part.rename(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('base', type=Path)
    parser.add_argument('update', type=Path)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = merge(args.base.resolve(), args.update.resolve(), args.keys.resolve(), args.output.resolve(),
                   lambda done, total: print(f'\r{done / total:6.1%}', end='', flush=True))
    print()
    print(json.dumps(dict(status='pass', files=len(report['files']), size=report['romfs_size'],
                          sha256=report['romfs_sha256'])))


if __name__ == '__main__':
    main()
