"""Make XC2's Xbox, PlayStation and keyboard button prompts (runtime/prompts.c, XC2_PROMPTS) from the player's archive.

XC2 draws its button prompts from textures: the [ML:icon icon=<name>] tags of the BDAT messages draw cells of the
512x256 BC3 atlas at the end of menu/image/button.wilay (8x4 cells of 64 pixels), and some layouts carry their own
button art. This tool reads those layouts from bf2.arh/bf2.ard in the merged RomFS, pastes Kenney's Input Prompts
(CC0, tools/data/kenney-input-prompts) over the Switch buttons and writes the layouts as loose files. The game asks
every add-on content mount for a menu file before it reads bf2.ard, so runtime/prompts.c serves these files to those
probes. The Switch prompts are the game's own and need no files.

The icons take the atlas's style: a white face, the letters or symbols in the game's blue (the face buttons in their
console's colours), a blue rim and a glow. Sets, one folder each:
  xbox, playstation                    gamepad layout Xbox (XC2_PAD_LAYOUT, runtime/input_map.h): Switch A is the
                                       bottom button, Xbox A, PlayStation cross
  xbox_nintendo, playstation_nintendo  Nintendo layout: Switch A is the right button, Xbox B, PlayStation circle
  keyboard                             the default keys and mouse buttons (runtime/input_map.c DEFAULT_KEYS)
Only the texture blocks under a replaced cell are re-encoded; every other byte of a layout stays the game's.

Input: build/extracted/<version>/romfs (romfs.bin and index.json, tools/extract.py). Output: <out>/<set>/menu/image/
<file>.wilay and <out>/manifest.tsv (set, path, size, SHA-256), by default build/<version>/prompts, which
tools/build_game.py copies next to xc2.exe. Nothing it writes may be committed. --png <dir> also writes every texture
before and after as PNG; --survey <dir> writes the textures of the image layouts whose names match --match as PNG, to
look for more button art. Runs on either Python: no third-party modules.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / 'tools' / 'data' / 'kenney-input-prompts'

# ---- bf2.arh / bf2.ard


class Archive:
    """bf2.arh: 'arh1', then nine u32 (string table length, node count, string table offset, its length, node table
    offset, its length, file table offset, file count, key). The string and node tables are XORed as u32 words with
    key ^ 0xF3F35353. The nodes are (next, prev) i32 pairs of a trie: the child of node cur for byte c is next ^ c when
    that node's prev is cur; a negative next is a leaf whose name ends at strings[-next], NUL-terminated and followed
    by the u32 file id. A file entry is u64 offset into bf2.ard, u32 compressed size, u32 size, u32 flags, u32 id."""

    def __init__(self, romfs):
        index = json.loads((romfs / 'index.json').read_text())
        if index.get('status') != 'pass':
            sys.exit(f'{romfs / "index.json"}: the extraction did not verify')
        where = {f['path']: (f['offset'], f['size']) for f in index['files'] if f['path'] in ('bf2.arh', 'bf2.ard')}
        if len(where) != 2:
            sys.exit(f'{romfs}: no bf2.arh/bf2.ard')
        self.image = romfs / 'romfs.bin'
        self.ard = where['bf2.ard'][0]
        with open(self.image, 'rb') as f:
            f.seek(where['bf2.arh'][0])
            h = bytearray(f.read(where['bf2.arh'][1]))
        magic, slen, nodes, soff, _, noff, nlen, foff, nfiles, key = struct.unpack_from('<4s9I', h, 0)
        if magic != b'arh1':
            sys.exit('bf2.arh: not an arh1 header')
        k = key ^ 0xF3F35353
        for off, ln in ((soff, slen), (noff, nlen)):
            words = struct.unpack_from(f'<{ln // 4}I', h, off)
            struct.pack_into(f'<{ln // 4}I', h, off, *(w ^ k for w in words))
        self.strings = bytes(h[soff:soff + slen])
        self.nodes = struct.unpack_from(f'<{2 * nodes}i', h, noff)
        self.files = [struct.unpack_from('<QIIII', h, foff + 24 * i) for i in range(nfiles)]

    def lookup(self, path):
        """The file id of path ('/menu/image/button.wilay'), None if the archive lacks it."""
        cur, n = 0, self.nodes
        for i, c in enumerate(path.encode('latin-1')):
            nxt = n[2 * cur]
            if nxt < 0:
                return self._leaf(-nxt, path[i:])
            j = nxt ^ c
            if not 0 <= j < len(n) // 2 or n[2 * j + 1] != cur:
                return None
            cur = j
        nxt = n[2 * cur]
        return self._leaf(-nxt, '') if nxt < 0 else None

    def _leaf(self, at, rest):
        end = self.strings.index(b'\0', at)
        if self.strings[at:end].decode('latin-1') != rest:
            return None
        return struct.unpack_from('<I', self.strings, end + 1)[0]

    def names(self):
        """Every path of the archive."""
        out, stack, n = [], [(0, '')], self.nodes
        while stack:
            cur, path = stack.pop()
            nxt = n[2 * cur]
            if nxt < 0:
                end = self.strings.index(b'\0', -nxt)
                out.append(path + self.strings[-nxt:end].decode('latin-1'))
                continue
            for c in range(1, 128):
                j = nxt ^ c
                if 0 <= j < len(n) // 2 and n[2 * j + 1] == cur:
                    stack.append((j, path + chr(c)))
        return sorted(out)

    def read(self, path):
        """The file's bytes. An 'xbc1' block: u32 type (1 = zlib), size, compressed size, the body at +48."""
        fid = self.lookup(path)
        if fid is None:
            sys.exit(f'bf2.arh: no {path}')
        off, csize, size, _, _ = self.files[fid]
        with open(self.image, 'rb') as f:
            f.seek(self.ard + off)
            d = f.read(max(csize, size) + 48)
        if d[:4] == b'xbc1':
            kind, usize, clen = struct.unpack_from('<III', d, 4)
            out = zlib.decompress(d[48:48 + clen]) if kind == 1 else d[48:48 + clen]
            if len(out) != usize:
                sys.exit(f'{path}: xbc1 block of {len(out)} bytes, {usize} expected')
            return out
        return d[:size or csize]


# ---- MIBL textures: the image (block-linear), then a 40-byte footer (size, alignment, width, height, depth,
# dimension, format, mip count, version, 'LBIM') at the end of the last 4 KiB page

FORMATS = {37: ('rgba8', 4, 1), 66: ('bc1', 8, 4), 68: ('bc3', 16, 4)}


class Texture:
    def __init__(self, blob, end):
        """The MIBL whose footer ends at blob[end]."""
        if blob[end - 4:end] != b'LBIM':
            raise ValueError('no MIBL footer')
        self.size, _, self.w, self.h, _, _, self.fmt, self.mips, _ = struct.unpack_from('<9I', blob, end - 40)
        self.kind, self.bpe, self.blk = FORMATS.get(self.fmt, (None, 0, 0))
        # The footer ends the image's pages, or a page of its own when the swizzled image fills them (button.wilay:
        # 131,072 bytes of image in 135,168; mnu001_btn.wilay: 1,024 in 4,096 with the footer)
        used = self.layout()[3] if self.kind else self.size
        self.start = end - self.size if used + 40 <= self.size else end - (self.size + 40 + 4095) // 4096 * 4096

    def layout(self):
        """Width and height in elements, the block height in GOBs, and the bytes the swizzled image spans. The block
        height is the one Tegra picks for the top level: 16, 8, 4, 2 or 1 GOBs by whether one and a half times the
        height in elements reaches 128, 64, 32 or 16 (tegra_swizzle block_height_mip0)."""
        wb, hb = (self.w + self.blk - 1) // self.blk, (self.h + self.blk - 1) // self.blk
        half = hb + hb // 2
        bh = 16 if half >= 128 else 8 if half >= 64 else 4 if half >= 32 else 2 if half >= 16 else 1
        gobs_x, gobs_y = (wb * self.bpe + 63) // 64, ((hb + 7) // 8 + bh - 1) // bh * bh
        return wb, hb, bh, gobs_x * gobs_y * 512

    def offsets(self):
        """The byte offset in the image of each element (block), in row order: block-linear, 64x8-byte GOBs."""
        wb, hb, bh, _ = self.layout()
        row = wb * self.bpe
        gobs_x = (row + 63) // 64
        out = []
        for y in range(hb):
            for xb in range(0, row, self.bpe):
                gob = (y // (8 * bh)) * bh * gobs_x * 512 + (xb // 64) * 512 * bh + ((y % (8 * bh)) // 8) * 512
                out.append(gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 +
                           (y % 2) * 16 + xb % 16)
        return wb, hb, out


def textures(blob):
    """The MIBL textures of a layout (LAHD), found by their footers."""
    out, at = [], blob.find(b'LBIM')
    while at >= 0:
        try:
            t = Texture(blob, at + 4)
            if 0 <= t.start and t.size and t.w and t.h:
                out.append(t)
        except (ValueError, struct.error):
            pass
        at = blob.find(b'LBIM', at + 4)
    return out


# ---- BC1 / BC3 blocks

def rgb565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def bc1_palette(c0, c1):
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1:
        return [a, b, tuple((2 * x + y + 1) // 3 for x, y in zip(a, b)), tuple((x + 2 * y + 1) // 3 for x, y in zip(a, b))]
    return [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]


def bc1_decode(block):
    """16 RGBA pixels; index 3 of a block with c0 <= c1 is transparent."""
    c0, c1, bits = struct.unpack_from('<HHI', block, 0)
    pal = bc1_palette(c0, c1)
    return [pal[k] + ((0,) if c0 <= c1 and k == 3 else (255,)) for k in ((bits >> 2 * i) & 3 for i in range(16))]


def alpha_palette(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1 + 3) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1 + 2) // 5 for i in range(1, 5)] + [0, 255]


def bc3_alpha_decode(block):
    bits = int.from_bytes(block[2:8], 'little')
    pal = alpha_palette(block[0], block[1])
    return [pal[(bits >> 3 * i) & 7] for i in range(16)]


def nearest(value, pal, dist):
    return min(range(len(pal)), key=lambda k: dist(value, pal[k]))


def bc3_alpha_encode(alpha):
    lo, hi = min(alpha), max(alpha)
    if lo == hi:
        return bytes((hi, lo)) + bytes(6)
    pal = alpha_palette(hi, lo)   # a0 > a1: eight steps between them
    bits = 0
    for i, a in enumerate(alpha):
        bits |= nearest(a, pal, lambda x, y: abs(x - y)) << 3 * i
    return bytes((hi, lo)) + bits.to_bytes(6, 'little')


def bc1_encode(pixels, weights):
    """16 RGB triples to a 4-colour BC1 block. The endpoints are the extremes of the pixels along their principal
    axis (weighted, so pixels the alpha hides count little); every pixel takes the nearest of the four colours."""
    total = sum(weights) or 1.0
    mean = [sum(p[c] * w for p, w in zip(pixels, weights)) / total for c in range(3)]
    cov = [[sum((p[i] - mean[i]) * (p[j] - mean[j]) * w for p, w in zip(pixels, weights)) for j in range(3)]
           for i in range(3)]
    axis = [1.0, 1.0, 1.0]
    for _ in range(8):   # power iteration
        v = [sum(cov[i][j] * axis[j] for j in range(3)) for i in range(3)]
        n = max(abs(x) for x in v)
        if n < 1e-9:
            break
        axis = [x / n for x in v]
    used = [p for p, w in zip(pixels, weights) if w > 0] or pixels
    proj = [sum((p[c] - mean[c]) * axis[c] for c in range(3)) for p in used]
    e0, e1 = used[proj.index(max(proj))], used[proj.index(min(proj))]
    q = lambda c: (round(c[0] * 31 / 255) << 11) | (round(c[1] * 63 / 255) << 5) | round(c[2] * 31 / 255)
    c0, c1 = q(e0), q(e1)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    pal = bc1_palette(c0, c1)
    bits = 0
    for i, p in enumerate(pixels):
        bits |= nearest(p, pal, lambda x, y: sum((a - b) ** 2 for a, b in zip(x, y))) << 2 * i
    return struct.pack('<HHI', c0, c1, bits)


class Image:
    """A texture's top level as rows of RGBA tuples, and its encoding back into the layout."""

    def __init__(self, blob, tex):
        self.tex = tex
        self.w, self.h = tex.w, tex.h
        self.px = [[(0, 0, 0, 0)] * tex.w for _ in range(tex.h)]
        wb, _, offs = tex.offsets()
        data = blob[tex.start:tex.start + tex.size]
        for n, o in enumerate(offs):
            el = data[o:o + tex.bpe]
            bx, by = n % wb, n // wb
            if tex.kind == 'rgba8':
                self.px[by][bx] = tuple(el)
                continue
            cols = bc1_decode(el[8:] if tex.kind == 'bc3' else el)
            alpha = bc3_alpha_decode(el) if tex.kind == 'bc3' else [c[3] for c in cols]
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < tex.w and y < tex.h:
                    self.px[y][x] = cols[i][:3] + (alpha[i],)
        self.dirty = set()   # (x, y) of the elements to encode again

    def paste(self, x0, y0, icon):
        """Replace the pixels at x0, y0 with icon (rows of RGBA tuples)."""
        for y, row in enumerate(icon):
            for x, p in enumerate(row):
                self.px[y0 + y][x0 + x] = p
                self.dirty.add(((x0 + x) // self.tex.blk, (y0 + y) // self.tex.blk))

    def encode(self, blob):
        out = bytearray(blob)
        tex = self.tex
        wb, _, offs = tex.offsets()
        for bx, by in sorted(self.dirty):
            n = by * wb + bx
            if tex.kind == 'rgba8':
                el = bytes(self.px[by][bx])
            else:
                pix = [self.px[min(by * 4 + i // 4, self.h - 1)][min(bx * 4 + i % 4, self.w - 1)] for i in range(16)]
                colour = bc1_encode([p[:3] for p in pix], [p[3] / 255 for p in pix])
                el = bc3_alpha_encode([p[3] for p in pix]) + colour if tex.kind == 'bc3' else colour
            out[tex.start + offs[n]:tex.start + offs[n] + tex.bpe] = el
        return bytes(out)

    def png(self, path, background=(64, 64, 64)):
        write_png(path, self.px, background)


# ---- PNG in and out (the vendored icons are 4- and 8-bit paletted PNGs)

def read_png(path):
    """Rows of RGBA tuples of a non-interlaced 8-bit RGBA or 1/2/4/8-bit paletted PNG."""
    d = path.read_bytes()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG')
    pos, idat, pal, trns = 8, b'', [], b''
    while pos < len(d):
        n, kind = struct.unpack_from('>I4s', d, pos)
        body = d[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            pal = [tuple(body[i:i + 3]) for i in range(0, n, 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if interlace or not ((ctype == 3 and depth in (1, 2, 4, 8)) or (ctype == 6 and depth == 8)):
        sys.exit(f'{path}: PNG colour type {ctype}, depth {depth}, interlace {interlace} not read')
    raw = zlib.decompress(idat)
    bpp = 4 if ctype == 6 else 1   # bytes per pixel for the filters (at least one)
    stride = (w * (32 if ctype == 6 else depth) + 7) // 8
    rows, prev, pos = [], bytes(stride), 0
    for _ in range(h):
        f, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = bytes(line)
        if ctype == 6:
            rows.append([tuple(line[4 * x:4 * x + 4]) for x in range(w)])
        else:
            idx = [(line[x * depth // 8] >> (8 - depth - x * depth % 8)) & ((1 << depth) - 1) for x in range(w)]
            rows.append([pal[k] + (trns[k] if k < len(trns) else 255,) for k in idx])
    return rows


def write_png(path, px, background=None):
    """A PNG of px (rows of RGBA tuples), composited on background (an RGB triple) if given."""
    data = bytearray()
    for row in px:
        data.append(0)
        for r, g, b, a in row:
            if background:
                r, g, b = ((c * a + k * (255 - a)) // 255 for c, k in zip((r, g, b), background))
                a = 255
            data += bytes((r, g, b, a))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', len(px[0]), len(px), 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(bytes(data), 9)) + chunk(b'IEND', b''))



# ---- icons

FILL = (247, 251, 255)   # the atlas's button faces
INK = (49, 138, 189)     # its letters, rims and glow
WHITE = (255, 255, 255)
TEAL = (34, 168, 190)    # the battle palette's buttons
BRAND = {   # the face buttons' colours, a little darker than the consoles' to read on white
    'xbox_button_a': (76, 160, 36), 'xbox_button_b': (214, 52, 52), 'xbox_button_x': (34, 112, 222),
    'xbox_button_y': (214, 160, 0),
    'playstation_button_cross': (72, 120, 222), 'playstation_button_circle': (222, 60, 72),
    'playstation_button_square': (206, 96, 176), 'playstation_button_triangle': (36, 168, 140),
}


def resample(src, w, h=None):
    """src (rows of RGBA) to w x h (h = w), averaging premultiplied colour over each target pixel's area."""
    h = h or w
    sh, sw = len(src), len(src[0])
    fx, fy = sw / w, sh / h
    out = []
    for y in range(h):
        row = []
        y0, y1 = y * fy, (y + 1) * fy
        for x in range(w):
            x0, x1 = x * fx, (x + 1) * fx
            acc, wsum = [0.0, 0.0, 0.0, 0.0], 0.0
            for sy in range(int(y0), min(sh, int(y1 + 0.999))):
                wy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(sw, int(x1 + 0.999))):
                    wt = wy * (min(x1, sx + 1) - max(x0, sx))
                    r, g, b, a = src[sy][sx]
                    acc[0] += r * a * wt
                    acc[1] += g * a * wt
                    acc[2] += b * a * wt
                    acc[3] += a * wt
                    wsum += wt
            row.append(tuple(round(c / acc[3]) for c in acc[:3]) + (round(acc[3] / wsum),) if acc[3] else (0, 0, 0, 0))
        out.append(row)
    return out


def blank(w, h):
    return [[(0, 0, 0, 0)] * w for _ in range(h)]


def over(dst, src, x0, y0):
    """src composited over dst (rows of RGBA) at x0, y0, in place."""
    for y, row in enumerate(src):
        for x, p in enumerate(row):
            if not p[3] or not (0 <= y0 + y < len(dst) and 0 <= x0 + x < len(dst[0])):
                continue
            q = dst[y0 + y][x0 + x]
            a = p[3] + q[3] * (255 - p[3]) / 255
            dst[y0 + y][x0 + x] = tuple(round((c * p[3] + d * q[3] * (255 - p[3]) / 255) / a)
                                        for c, d in zip(p[:3], q[:3])) + (round(a),)
    return dst


def icon(kit, name, size):
    return resample(read_png(ICONS / kit / f'{name}.png'), size)


# keyboard: the sticks and the d-pad as small keys side by side; (key, x, y) on a 64-pixel cell, keys of 26 pixels
KEY_GROUPS = {
    'lstick': (('keyboard_w', 19, 4), ('keyboard_a', 0, 30), ('keyboard_s', 19, 30), ('keyboard_d', 38, 30)),
    'dpad': (('keyboard_2', 19, 0), ('keyboard_1', 0, 19), ('keyboard_3', 38, 19), ('keyboard_4', 19, 38)),
    'vertical': (('keyboard_2', 18, 3), ('keyboard_4', 18, 33)),
    'horizontal': (('keyboard_1', 3, 18), ('keyboard_3', 33, 18)),
}


def source(kit, name, size):
    """The Kenney icon (or group of keys) at size x size."""
    if name in KEY_GROUPS:
        s = size / 64
        out = blank(size, size)
        for key, x, y in KEY_GROUPS[name]:
            over(out, icon(kit, key, round(28 * s)), round(x * s), round(y * s))
        return out
    return icon(kit, name, size)


def styled(src, face=FILL, letters=INK, rim=INK, scale=1.0, glow=True):
    """A Kenney icon (white shapes with the letters cut out) in the atlas's style. Inside the shape's outline the
    white becomes face, the cut-out letters letters, any other colour (the highlighted arm of a d-pad, the pressed
    mouse button) rim; around it lie a rim of two pixels at 64 (scale 1) and a glow of six, both in rim."""
    h, w = len(src), len(src[0])
    outside = [[False] * w for _ in range(h)]
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:   # the transparent pixels joined to the border
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not outside[y][x] and src[y][x][3] < 128:
            outside[y][x] = True
            stack += ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))
    dist = [[99.0 if outside[y][x] else 0.0 for x in range(w)] for y in range(h)]
    for _ in range(2):   # chamfer distance to the shape, forwards and backwards
        for ys, steps in ((range(h), ((-1, 0, 1), (0, -1, 1), (-1, -1, 1.414), (1, -1, 1.414))),
                          (range(h - 1, -1, -1), ((1, 0, 1), (0, 1, 1), (1, 1, 1.414), (-1, 1, 1.414)))):
            for y in ys:
                for x in (range(w) if steps[0][0] < 0 else range(w - 1, -1, -1)):
                    for dx, dy, c in steps:
                        if 0 <= x + dx < w and 0 <= y + dy < h and dist[y + dy][x + dx] + c < dist[y][x]:
                            dist[y][x] = dist[y + dy][x + dx] + c
    r_in, r_out = 2.0 * scale, (8.0 if glow else 2.6) * scale
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = src[y][x]
            col = face if min(r, g, b) > 180 else rim
            t = a / 255
            if not outside[y][x]:   # the face, and the letters cut out of it
                row.append(tuple(round(l + (f - l) * t) for l, f in zip(letters, col)) + (255,))
                continue
            d = dist[y][x]
            halo = 1.0 if d <= r_in else max(0.0, 1 - (d - r_in) / (r_out - r_in)) ** 1.6 * (0.9 if glow else 1.0)
            ga = halo * (1 - t)   # the rim and glow under the shape's anti-aliased edge
            al = t + ga
            row.append(tuple(round((f * t + i * ga) / al) for f, i in zip(col, rim)) + (round(al * 255),)
                       if al > 0 else rim + (0,))
        out.append(row)
    return out


def darker(c, k=0.45):
    return tuple(round(v * k) for v in c)


# ---- the sets: the icon of each Switch button

PAD = {
    'xbox': {'L': 'xbox_lb', 'R': 'xbox_rb', 'ZL': 'xbox_lt', 'ZR': 'xbox_rt', 'plus': 'xbox_button_menu',
             'minus': 'xbox_button_view', 'home': 'xbox_guide', 'capture': 'xbox_button_share', 'L3': 'xbox_ls',
             'R3': 'xbox_rs', 'lstick': 'xbox_stick_l', 'rstick': 'xbox_stick_r', 'dpad': 'xbox_dpad',
             'up': 'xbox_dpad_up', 'down': 'xbox_dpad_down', 'left': 'xbox_dpad_left', 'right': 'xbox_dpad_right',
             'vertical': 'xbox_dpad_vertical', 'horizontal': 'xbox_dpad_horizontal'},
    'playstation': {'L': 'playstation_trigger_l1', 'R': 'playstation_trigger_r1', 'ZL': 'playstation_trigger_l2',
                    'ZR': 'playstation_trigger_r2', 'plus': 'playstation5_button_options',
                    'minus': 'playstation5_button_create', 'capture': 'playstation4_button_share',
                    'L3': 'playstation_button_l3', 'R3': 'playstation_button_r3', 'lstick': 'playstation_stick_l',
                    'rstick': 'playstation_stick_r', 'dpad': 'playstation_dpad', 'up': 'playstation_dpad_up',
                    'down': 'playstation_dpad_down', 'left': 'playstation_dpad_left',
                    'right': 'playstation_dpad_right', 'vertical': 'playstation_dpad_vertical',
                    'horizontal': 'playstation_dpad_horizontal'},
}
# the bottom, right, left and top face buttons: Switch A, B, X, Y in the Xbox layout
FACE = {'xbox': ('xbox_button_a', 'xbox_button_b', 'xbox_button_x', 'xbox_button_y'),
        'playstation': ('playstation_button_cross', 'playstation_button_circle', 'playstation_button_square',
                        'playstation_button_triangle')}
# runtime/input_map.c DEFAULT_KEYS (Switch home, capture, SL and SR have no key)
KEYS = {'A': 'keyboard_e', 'B': 'keyboard_shift', 'X': 'keyboard_space', 'Y': 'mouse_left', 'L': 'keyboard_q',
        'R': 'keyboard_r', 'ZL': 'keyboard_z', 'ZR': 'keyboard_f', 'plus': 'keyboard_tab', 'minus': 'keyboard_m',
        'L3': 'keyboard_ctrl', 'R3': 'keyboard_c', 'lstick': 'lstick', 'rstick': 'mouse_move', 'dpad': 'dpad',
        'up': 'keyboard_2', 'down': 'keyboard_4', 'left': 'keyboard_1', 'right': 'keyboard_3',
        'vertical': 'vertical', 'horizontal': 'horizontal'}


def pad_set(kit, nintendo):
    face = FACE[kit]
    if nintendo:   # Switch A B X Y on the right, bottom, top and left buttons
        face = (face[1], face[0], face[3], face[2])
    return kit, dict(zip('ABXY', face), **PAD[kit])


SETS = {
    'xbox': pad_set('xbox', False),
    'xbox_nintendo': pad_set('xbox', True),
    'playstation': pad_set('playstation', False),
    'playstation_nintendo': pad_set('playstation', True),
    'keyboard': ('keyboard', KEYS),
}


class Painter:
    """The styled icons of one set, cached by everything that shapes them."""

    def __init__(self, kit_set):
        self.kit, self.names = SETS[kit_set]
        self.cache = {}

    def has(self, button):
        return button in self.names

    def atlas_cell(self, button, size):
        name = self.names[button]
        return self._get(('atlas', name, size), lambda: styled(source(self.kit, name, size),
                                                                  letters=BRAND.get(name, INK), scale=size / 64))

    def palette(self, button, size, w=None, h=None):
        """The battle palette's style: a solid button in its brand's colour (teal for the rest) with white letters."""
        name = self.names[button]
        face = BRAND.get(name, TEAL)

        def make():
            src = source(self.kit, name, size)
            if w:
                src = resample(src, w, h)
            return styled(src, face=face, letters=WHITE, rim=darker(face), scale=size / 64, glow=False)
        return self._get(('palette', name, size, w, h), make)

    def _get(self, key, make):
        if key not in self.cache:
            self.cache[key] = make()
        return self.cache[key]


# ---- the layouts

# button.wilay and font_icon.wilay: an atlas of 8x4 cells, 64 and 24 pixels; (column, row) of each button
CELLS = {
    'dpad': (0, 0), 'capture': (1, 0), 'home': (2, 0), 'minus': (3, 0), 'plus': (4, 0), 'X': (5, 0), 'Y': (6, 0),
    'A': (7, 0), 'B': (0, 1), 'L3': (1, 1), 'R3': (2, 1), 'lstick': (3, 1), 'rstick': (4, 1), 'L': (5, 1),
    'R': (6, 1), 'ZL': (7, 1), 'ZR': (0, 2), 'ZL2': (3, 2), 'ZR2': (4, 2), 'up': (0, 3), 'down': (1, 3),
    'left': (2, 3), 'right': (3, 3), 'vertical': (4, 3), 'horizontal': (5, 3),
}   # (1, 2) and (2, 2) are SL and SR, Joy-Con only: they stay, as do the buttons a set has no icon for


def atlas(imgs, p):
    img = imgs[-1]
    size = img.w // 8
    for cell, (col, row) in CELLS.items():
        button = cell[:2] if cell in ('ZL2', 'ZR2') else cell
        if p.has(button):
            img.paste(col * size, row * size, p.atlas_cell(button, size))


def palette(imgs, p):
    """mnu032_button_icon(_new).wilay: the battle palette's A B X Y, 72-pixel cells in a row."""
    img = imgs[-1]
    for k, button in enumerate('ABXY'):
        img.paste(k * 72, 0, p.palette(button, 72))


def push(imgs, p):
    """mnu032_button_push_main.wilay: textures 2 to 9 are A A B B X X Y Y as discs tilted away (96x80), each over
    its rim; the button lies flat at 56x34 over a darker copy 7 pixels lower."""
    for k, button in enumerate('AABBXXYY'):
        img = imgs[2 + k]
        top = p.palette(button, 56, 56, 34)
        side = [[darker(q[:3], 0.6) + (q[3],) for q in row] for row in top]
        canvas = over(over(blank(img.w, img.h), side, (img.w - 56) // 2, 26), top, (img.w - 56) // 2, 19)
        img.paste(0, 0, canvas)


def circle(imgs, p):
    """mnu032_btn_circle.wilay: a white B button (radius 37 about the centre) inside a ring; the button is drawn
    again over the ring's inside."""
    img = imgs[-1]
    cx, cy = img.w // 2, img.h // 2
    ring = img.px[cy - 46][cx]   # the ring's inside, above the button
    base = [[ring if (x - cx + 0.5) ** 2 + (y - cy + 0.5) ** 2 <= 40 ** 2 else img.px[y][x]
             for x in range(img.w)] for y in range(img.h)]
    over(base, p.atlas_cell('B', 96), cx - 48, cy - 48)
    img.paste(cx - 48, cy - 48, [row[cx - 48:cx + 48] for row in base[cy - 48:cy + 48]])


def plus(imgs, p):
    """mnu001_btn.wilay: the plus button, 32x32."""
    imgs[-1].paste(0, 0, p.atlas_cell('plus', 32))


LAYOUTS = {
    '/menu/image/button.wilay': atlas,
    '/menu/image/font_icon.wilay': atlas,
    '/menu/image/mnu032_button_icon.wilay': palette,
    '/menu/image/mnu032_button_icon_new.wilay': palette,
    '/menu/image/mnu032_button_push_main.wilay': push,
    '/menu/image/mnu032_btn_circle.wilay': circle,
    '/menu/image/mnu001_btn.wilay': plus,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v301')
    ap.add_argument('--romfs', type=Path, help='default build/extracted/<version>/romfs')
    ap.add_argument('--out', type=Path, help='default build/<version>/prompts')
    ap.add_argument('--png', type=Path, help='also write the textures before and after as PNG here')
    ap.add_argument('--survey', type=Path, help='write the textures of the image layouts matching --match as PNG here')
    ap.add_argument('--match', default=r'button|btn|key|guide|icon|pad|operation|tutorial')
    args = ap.parse_args()
    arc = Archive(args.romfs or ROOT / 'build/extracted' / args.version / 'romfs')

    if args.survey:
        pat = re.compile(args.match)
        names = [n for n in arc.names() if n.startswith('/menu/image/') and n.endswith('.wilay') and pat.search(n)]
        for n in names:
            blob = arc.read(n)
            for k, t in enumerate(textures(blob)):
                if t.kind:
                    Image(blob, t).png(args.survey / f'{Path(n).stem}.{k}.png')
                else:
                    print(f'{n}: texture {k} format {t.fmt} not decoded')
        print(f'{len(names)} layouts surveyed into {args.survey}')
        return

    out = args.out or ROOT / 'build' / args.version / 'prompts'
    lines = []
    painters = {s: Painter(s) for s in SETS}
    for path, redraw in LAYOUTS.items():
        blob = arc.read(path)
        texs = textures(blob)
        if any(not t.kind for t in texs):
            sys.exit(f'{path}: a texture format this tool does not write')
        stem = Path(path).stem
        if args.png:
            for k, t in enumerate(texs):
                Image(blob, t).png(args.png / f'{stem}.{k}.game.png')
        for kit_set, painter in painters.items():
            imgs = [Image(blob, t) for t in texs]
            redraw(imgs, painter)
            new = blob
            for img in imgs:
                new = img.encode(new)
            if args.png:
                for k, t in enumerate(texs):
                    if imgs[k].dirty:
                        Image(new, t).png(args.png / f'{stem}.{k}.{kit_set}.png')
            dst = out / kit_set / path.lstrip('/')
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(new)
            lines.append(f'{kit_set}\t{path.lstrip("/")}\t{len(new)}\t{hashlib.sha256(new).hexdigest()}')
        print(f'{path}: {len(painters)} sets')
    (out / 'manifest.tsv').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
