"""Census of every Maxwell shader in the extracted XC2 or XCX RomFS and ExeFS, and an offline translation pass.

XC2 keeps its shaders in Monolith shader archives inside bf2.ard (xc2_inventory, spch_shaders); the probes and the
pipeline self-test read build/v301/shaders (runtime/gfx.h GFX_CENSUS_DIR):

    .venv-py314\\Scripts\\python tools\\shader_census.py --version v301

The rest of this docstring describes the XCX path.

Port of MacNativeGames games/totk/scripts/prepare_shader_seed.py (inventory, bnsh_shaders, sharcb_shaders, sarc) and the
package walk of locate_shaders.py @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors). Differences: no
shared shader list, no spirv-cross, no bind-state variants; the module scan reads the NSO files under exefs/ directly;
a control section must start with CONTROL_MAGIC (sharcb_shaders says why).

Reads build/extracted/<version>/ (RomFS image plus index.json as tools/romfs_index.py reads them, and exefs/). Writes under
build/<version>/shaders/ only:
    code/<key>.code     translator input (graphics: SPH + instructions, compute: instructions)
    jobs.json           key -> job, source path, origin (bnsh|sharcb|main)
    spv/<key>.spv       with --translate
    failures.txt        with --translate, when something failed: groups by message

The key is sha256(control[:0x880] + code), as in MNG. Needs Python 3.14 (compression.zstd), so run in .venv-py314:

    .venv-py314\\Scripts\\python tools\\shader_census.py --version v121 --translate
"""
import argparse
import compression.zstd as zstd
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from collections import Counter, defaultdict
from pathlib import Path

import lz4.block

ROOT = Path(__file__).resolve().parent.parent
XLATE = ROOT / 'build' / 'xlate' / ('xc2_xlate.exe' if os.name == 'nt' else 'xc2_xlate')
SPIRV_VAL = Path(r'C:\msys64\ucrt64\bin\spirv-val.exe') if os.name == 'nt' else Path(shutil.which('spirv-val') or '/usr/bin/spirv-val')
ZSTD = b'\x28\xb5\x2f\xfd'
# NVN program control section magic, u32 0x98761234 (d2r-mobile dm/gfx/tools/shader_map.py)
CONTROL_MAGIC = b'\x34\x12\x76\x98'
STAGE_IDS = {0: 'vs', 1: 'ps', 5: 'cs'}
MAX_WORKERS = 20


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset + size]


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian + 'H', data, header + 6)[0]
    names = header + 12 + count * 16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian + '4I', checked(data, header + 12 + i * 16, 16))
        off = names + (attribute & 0xffffff) * 4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start + lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start + lo, hi - lo)
    return result


def bnsh_shaders(data):
    """BNSH absolute offsets are relative to each embedded BNSH, not BFSHA."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
        checked(data, program, 56)
        for stage, relative in [('vs', 8), ('ps', 40), ('cs', 48)]:
            offset = struct.unpack_from('<Q', data, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset + 8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < 0x880:
                continue   # the SDK utility BNSH uses an older, shorter control layout
            control = control[:0x880]   # some are 0x88c; NVN and the runtime's key read 0x880
            if control[:4] != CONTROL_MAGIC:
                raise ValueError('NVN control section without its magic')
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            yield digest(control + code[:length]), stage, control, code[:length]


def sharcb_shaders(data, rejected):
    """agl shader packages (.sharcb, magic BAHS): records <u64 code offset in the pool, u32 control size (0x880 or
    0x88c), i32 offset of the control from the record>; a record may reuse an earlier record's control.

    The scan finds records by their size word, so it also meets 16-byte groups that only look like one. Without the
    magic check, 1.2.1 yields 87 such keys: each has control offset 0 (the "control" starts at the record itself, so its
    first 8 bytes are the code offset) and no CONTROL_MAGIC. The game hands NVN real control sections and the runtime
    key hashes those, so no draw can ever produce these keys. Their code is cut from the wrong place: they were all 67
    translation failures of 1.2.1 (IndexOutOfRange in ShaderHeader.ctor and ArgumentOutOfRange in Decoder.FillBlock,
    on 0 to 4,718 bytes of code). MNG's scan (prepare_shader_seed.py @ b75be8cf) keeps them; the 7 that translate are
    V rows in MNG's list only because shader_list.py add_cache() folds the seed's own .variants files back in. A
    candidate without CONTROL_MAGIC is skipped and counted in rejected[0]."""
    if len(data) < 32 or data[:4] != b'BAHS':
        raise ValueError('expected an agl shader package')
    start, pool_size, pool = struct.unpack_from('<3I', data, 20)
    if not 32 <= start <= pool or pool + pool_size > len(data):
        raise ValueError('agl shader package layout outside the file')
    seen = set()
    for size in (b'\x80\x08\x00\x00', b'\x8c\x08\x00\x00'):
        at = start
        while (at := data.find(size, at, pool)) >= 0:
            record, at = at - 8, at + 4
            if record < start or record % 4:
                continue
            code_off = struct.unpack_from('<Q', data, record)[0]
            control_at = record + struct.unpack_from('<i', data, record + 12)[0]
            if not start <= control_at <= pool - 0x880 or code_off >= pool_size:
                continue
            control = data[control_at:control_at + 0x880]
            if control[:4] != CONTROL_MAGIC:
                rejected[0] += 1
                continue
            c1, length = struct.unpack_from('<II', control, 0x700)
            stage = STAGE_IDS.get(struct.unpack_from('<I', control, 0x714)[0])
            if not stage or not 0 < length <= pool_size - code_off or c1 > length:
                continue
            code = data[pool + code_off:pool + code_off + length]
            key = digest(control + code)
            if key not in seen:
                seen.add(key)
                yield key, stage, control, code


def nso_image(path):
    """The decompressed text, rodata and data segments of an NSO, concatenated (enough to find embedded BNSH files)."""
    raw = path.read_bytes()
    if raw[:4] != b'NSO0':
        raise ValueError(f'{path} is not an NSO')
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    parts = []
    for i in range(3):
        file_off, _, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + comp_size]
        parts.append(lz4.block.decompress(data, uncompressed_size=dec_size) if flags & (1 << i) else data)
    return b''.join(parts)


def startup_package(name):
    """The RomFS files MNG's startup shaders come from; effects and UI layouts carry more."""
    return (name == 'Lib/NintendoSDK/TextureShader.bnsh' or name == 'Pack/Bootup.Nin_NX_NVN.pack.zs' or
            name.startswith('Shader/') and name.endswith('.zs'))


def spch_shaders(data, at, name):
    """The shaders of the Monolith shader archive (magic HCPS) at data[at:]. Header after the magic: u32 version, then
    (offset, count) of the program table (u32 Slct offset, u32 flags each), (offset, count) of a table not read here, and
    (offset, size) of the Slct, xV4 (code) and control sections, all relative to the magic. A Slct ('SLCT') holds at +40
    the offset and size of its controls in the control section and of its code in the xV4 section. Its variants lie
    there back to back: each stage's NVN control section, sized by its own u32 at +0x20 (0x838 or 0x848), and that
    stage's code, sized by the control's code length (+0x704). Each walk must end exactly at the Slct's code size.
    The control handed to NVN is the 0x880 bytes from its start, as the game passes it. Yields (key, stage, control,
    code, source); the source names the variant, so a variant's vertex and fragment shader share it."""
    if len(data) < at + 48:
        raise ValueError('shader archive header outside the data')
    _, _, poff, pcount, _, _, soff, _, xoff, xsize, coff, csize = struct.unpack_from('<4s11I', data, at)
    if at + max(poff + 8 * pcount, xoff + xsize, coff + csize) > len(data):
        raise ValueError('shader archive sections outside the data')
    slcts = sorted({struct.unpack_from('<I', data, at + poff + 8 * i)[0] for i in range(pcount)})
    for n, s in enumerate(slcts):
        s = at + soff + s
        if data[s:s + 4] != b'SLCT':
            raise ValueError('shader archive program without SLCT')
        co, cs, xo, xs = struct.unpack_from('<4I', data, s + 40)
        c, k, end, variant, last = at + coff + co, at + xoff + xo, at + coff + co + cs, 0, -1
        if co + cs > csize or xo + xs > xsize:
            raise ValueError('SLCT outside its sections')
        while c < end:
            control = data[c:c + 0x880].ljust(0x880, b'\0')
            size, length, stage = (struct.unpack_from('<I', control, o)[0] for o in (0x20, 0x704, 0x714))
            if control[:4] != CONTROL_MAGIC or not 0x700 < size <= 0x880 or k + length > at + xoff + xo + xs:
                raise ValueError('SLCT control without its magic or outside its code')
            variant += stage <= last
            last = stage
            if stage in STAGE_IDS:
                code = data[k:k + length]
                yield digest(control + code), STAGE_IDS[stage], control, code, f'{name}#{at:x}/{n}/{variant}'
            c += size
            k += length
        if k != at + xoff + xo + xs:
            raise ValueError('SLCT code does not add up to its size')


def xc2_inventory(romfs, rejected):
    """XC2: every file of bf2.arh/bf2.ard (tools/xc2_prompts.py Archive) that carries shader archives. The engine's
    shader libraries (.wishp) are the startup shaders; models (.wimdo, .wismt), map models (.wismda) and effects
    (.wiefb) carry their own, the .wismt ones inside zlib xbc1 blocks. rejected[0] counts archives that do not parse."""
    from xc2_prompts import Archive
    archive = Archive(romfs)
    shaders = {}
    for name in archive.names():
        ext = name.rsplit('.', 1)[-1]
        if ext not in ('wishp', 'wimdo', 'wismt', 'wismda', 'wiefb'):
            continue
        data = archive.read(name)
        blobs, pos = [data], 0
        while (pos := data.find(b'xbc1', pos)) >= 0:
            kind, size, csize = struct.unpack_from('<III', data, pos + 4) if pos + 16 <= len(data) else (0, 0, 0)
            pos += 4
            if kind == 1 and size <= 512 << 20:
                try:
                    blob = zlib.decompress(data[pos + 44:pos + 44 + csize])
                except zlib.error:
                    continue
                if len(blob) == size and b'HCPS' in blob:
                    blobs.append(blob)
        for blob in blobs:
            at = 0
            while (at := blob.find(b'HCPS', at)) >= 0:
                try:
                    for key, stage, control, code, source in spch_shaders(blob, at, name):
                        shaders.setdefault(key, dict(stage=stage, control=control, code=code, source=source,
                                                     origin=ext, startup=ext == 'wishp'))
                except (ValueError, struct.error):
                    rejected[0] += 1
                at += 4
    return shaders


def inventory(romfs, exefs):
    """Every unique shader: key -> dict(stage, control, code, source, origin, startup). As in MNG, a shader is a startup
    shader when it comes from an agl package, a game module, or a startup_package() RomFS file; the others come from
    effect and UI layout resources, which MNG translates only for bind states players have met. Also returns the number
    of .sharcb record candidates rejected for a missing control magic (see sharcb_shaders)."""
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    shaders = {}
    rejected = [0]
    if 'bf2.arh' in entries:
        return xc2_inventory(romfs, rejected), rejected[0]

    def add(origin, name, found, startup):
        for key, stage, control, code in found:
            s = shaders.setdefault(key, dict(stage=stage, control=control, code=code, source=name, origin=origin,
                                             startup=startup))
            if startup and not s['startup']:
                s.update(source=name, origin=origin, startup=True)

    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise ValueError('short RomFS read')
            return raw

        dictionaries = {}
        for raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
            d = zstd.ZstdDict(raw)
            dictionaries[d.dict_id] = d

        def decode(raw):
            info = zstd.get_frame_info(raw)
            if info.decompressed_size is None or info.decompressed_size > 512 << 20:
                raise ValueError('unbounded shader package')
            return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)

        def visit(origin, name, data, startup, depth=0):
            if data[:4] == b'BAHS':
                add('sharcb', name, sharcb_shaders(data, rejected), True)
                return
            if data[:4] == b'SARC':
                if depth > 4:
                    return
                for member, raw in sarc(data).items():
                    visit(origin, name + '/' + member, decode(raw) if raw[:4] == ZSTD else raw, startup, depth + 1)
                return
            pos = 0   # BFSHA and BNSH files, effect and layout resources: complete, independently addressed BNSH files
            while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
                size = struct.unpack_from('<I', checked(data, pos + 28, 4))[0]
                add(origin, name, bnsh_shaders(checked(data, pos, size)), startup)
                pos += max(size, 8)

        for name in sorted(entries):
            raw = read(name)
            if raw[:4] not in (ZSTD, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
                continue
            visit('bnsh', name, decode(raw) if raw[:4] == ZSTD else raw, startup_package(name))
    for module in sorted(exefs.iterdir()):
        if module.name in ('main', 'sdk', 'subsdk0', 'rtld'):
            visit('main', 'module:' + module.name, nso_image(module), True)
    return shaders, rejected[0]


def write_shaders(shaders, out, everything):
    """Write code files and jobs.json; returns {key: dict(job, source, origin)}."""
    (out / 'code').mkdir(parents=True, exist_ok=True)
    jobs = {}
    for key, s in sorted(shaders.items()):
        if not s['startup'] and not everything:
            continue
        control, code, stage = s['control'], s['code'], s['stage']
        c1, length = struct.unpack_from('<II', control, 0x700)
        body = code[0 if stage == 'cs' else 0x30:length]
        path = out / 'code' / f'{key}.code'
        if not path.is_file() or path.read_bytes() != body:
            path.write_bytes(body)
        job = dict(key=key, stage=stage, code=str(path), c1=code[c1:length].hex(), debug=False)
        if stage == 'cs':
            job['local'] = list(struct.unpack_from('<III', control, 0x748))
        jobs[key] = dict(job=job, source=s['source'], origin=s['origin'], startup=s['startup'])
    (out / 'jobs.json').write_text(json.dumps(jobs, indent=0), encoding='utf-8')
    return jobs


def worker(keys, jobs, spv_dir):
    """One --serve process for a slice of keys. Returns (translated, failures {key: message}, errors {key: message})."""
    failures, errors, ok = {}, {}, 0
    process = subprocess.Popen([str(XLATE), '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
    try:
        for key in keys:
            process.stdin.write(json.dumps({'job': jobs[key]['job'], 'out': str(spv_dir)}) + '\n')
            process.stdin.flush()
            reply = process.stdout.readline().rstrip('\n')
            if not reply:
                failures[key] = 'translator service exited'
                break
            if not reply.startswith('ok '):
                failures[key] = reply.split(' ', 2)[2] if reply.count(' ') >= 2 else reply
                continue
            v = subprocess.run([str(SPIRV_VAL), '--target-env', 'vulkan1.3', str(spv_dir / f'{key}.spv')],
                               capture_output=True, text=True)
            if v.returncode:
                text = (v.stderr or v.stdout).strip()
                errors[key] = text.splitlines()[0] if text else 'spirv-val failed'
            else:
                ok += 1
    finally:
        process.stdin.close()
        process.wait()
    return ok, failures, errors


def normalize(message):
    return re.sub(r'\b\d+\b|0x[0-9a-fA-F]+', 'N', message)[:300]


def translate(jobs, out):
    spv_dir = out / 'spv'
    spv_dir.mkdir(exist_ok=True)
    keys = sorted(jobs)
    count = max(1, min(MAX_WORKERS, len(keys)))
    slices = [keys[i::count] for i in range(count)]
    ok, failures, errors = 0, {}, {}
    with concurrent.futures.ThreadPoolExecutor(count) as pool:
        for n, f, e in pool.map(lambda s: worker(s, jobs, spv_dir), slices):
            ok += n
            failures.update(f)
            errors.update(e)
    if failures or errors:
        lines = []
        for title, table in (('translate failures', failures), ('spirv-val errors', errors)):
            groups = defaultdict(list)
            for key, message in table.items():
                groups[normalize(message)].append((key, message))
            lines.append(f'== {title}: {len(table)} in {len(groups)} groups')
            for _, members in sorted(groups.items(), key=lambda g: -len(g[1])):
                lines.append(f'{len(members)}\t{members[0][1][:300]}')
                lines.extend(f'\texample {key}' for key, _ in sorted(members)[:2])
        (out / 'failures.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    else:
        (out / 'failures.txt').unlink(missing_ok=True)
    return ok, len(failures), len(errors)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', required=True, help='v301 for XC2, v121 for XCX (build/extracted/<version>)')
    ap.add_argument('--all', action='store_true', help='also the layout and effect shaders MNG leaves to bind-state variants')
    ap.add_argument('--translate', action='store_true', help='translate every shader and run spirv-val')
    args = ap.parse_args()
    t0 = time.monotonic()
    extracted = ROOT / 'build' / 'extracted' / args.version
    out = ROOT / 'build' / args.version / 'shaders'
    out.mkdir(parents=True, exist_ok=True)
    shaders, rejected = inventory(extracted / 'romfs', extracted / 'exefs')
    jobs = write_shaders(shaders, out, args.all)
    print(f'{len(shaders)} unique shaders found in all, {len(jobs)} selected; {rejected} .sharcb record candidates '
          f'(XC2: shader archives) rejected', flush=True)
    origins = Counter(j['origin'] for j in jobs.values())
    summary = f'shaders {len(jobs)} ({", ".join(f"{o} {n}" for o, n in sorted(origins.items()))})'
    print(f'extracted {summary} in {time.monotonic() - t0:.1f} s', flush=True)
    if not args.translate:
        print(f'{summary}, total {time.monotonic() - t0:.1f} s')
        return 0
    if not XLATE.is_file() or not SPIRV_VAL.is_file():
        sys.exit(f'missing {XLATE} or {SPIRV_VAL}')
    ok, failed, bad = translate(jobs, out)
    print(f'{summary}, translated {ok}, failed {failed}, spirv-val errors {bad}, total {time.monotonic() - t0:.1f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
