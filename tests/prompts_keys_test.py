"""The keyboard prompt tile table: key names, tile sizes, bind defaults and patch offsets. No game files; the layouts
are stand-ins whose block offsets are identity maps, so the table the runtime reads can be checked end to end."""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import xc2_prompts as p  # noqa: E402


class FakeTex:
    def __init__(self, w, h):
        self.w, self.h, self.bpe, self.start, self.kind = w, h, 16, 0, 'bc3'
        self.size, self.blk = w * h, 4

    def offsets(self):
        wb, hb = self.w // 4, self.h // 4
        return wb, hb, [16 * i for i in range(wb * hb)]


FAKE = {
    '/menu/image/button.wilay': [FakeTex(512, 256)],
    '/menu/image/font_icon.wilay': [FakeTex(192, 96)],
    '/menu/image/mnu032_button_icon.wilay': [FakeTex(288, 72)],
    '/menu/image/mnu032_button_icon_new.wilay': [FakeTex(288, 72)],
    '/menu/image/mnu032_button_push_main.wilay': [FakeTex(96, 80) for _ in range(10)],
    '/menu/image/mnu032_btn_circle.wilay': [FakeTex(168, 168)],
    '/menu/image/mnu001_btn.wilay': [FakeTex(32, 32)],
}


class FakeArc:
    def __init__(self, index):
        self.index = index

    def read(self, path):
        return bytes([self.index]) + bytes(168 * 168)


def main():
    # The generator and input_map.c must agree on the keys a player can bind.
    src = (ROOT / 'runtime' / 'input_map.c').read_text(encoding='utf-8')
    block = re.search(r'static const char \*const key_names\[KEY_COUNT\] = \{(.*?)\};', src, re.S).group(1)
    assert re.findall(r'"([a-z0-9_]+)"', block) == p.KEY_NAMES, 'key_names in input_map.c and keys.dat differ'
    assert set(p.BAKED_KEY) == set(p.DEFAULT_KEYS) == set(p.ACTION_IDS)
    assert all(k in p.KEY_NAMES for keys in p.DEFAULT_KEYS.values() for k in keys)
    assert all(k in p.KEY_NAMES for k in p.BAKED_KEY.values())

    # Every synthetic keycap has a label, and every style of every key is a whole number of BC3 blocks.
    for name in p.KEY_NAMES:
        cap = p.keycap(name)
        assert len(cap) == 64 and len(cap[0]) == 64
        assert any(q[3] == 255 for row in cap for q in row), f'{name}: no keycap drawn'
    for s in range(len(p.TILE_STYLES)):
        for name in p.KEY_NAMES:
            w, h = p.TILE_STYLES[s][1:]
            base = p.blank(96, 96) if s == 4 else None
            assert len(p.encode_blocks(p.tile_image(name, s, base))) == w * h, f'{name} style {s}: wrong tile size'

    # Write a table against fake layouts, then read it the way prompts_keys.c does.
    out = ROOT / 'build' / 'test' / 'prompts_keys'
    keyboard = out / 'keyboard'
    keyboard.mkdir(parents=True, exist_ok=True)
    order = list(FAKE)
    old = p.textures
    p.textures = lambda blob: FAKE[order[blob[0]]]
    try:
        for path, texs in FAKE.items():
            size = max(t.w * t.h for t in texs)
            (keyboard / path.lstrip('/')).parent.mkdir(parents=True, exist_ok=True)
            (keyboard / path.lstrip('/')).write_bytes(bytes([order.index(path)]) + bytes(size))
        p.write_key_tiles(keyboard, FakeArc(order.index('/menu/image/mnu032_btn_circle.wilay')))
    finally:
        p.textures = old

    blob = (keyboard / 'keys.dat').read_bytes()
    magic, version, nkeys, nstyles, nfiles, npatches, tile_hash = struct.unpack_from('<4sHHHHII', blob)
    assert (magic, version) == (b'XC2K', 2)
    assert (nkeys, nstyles, nfiles) == (len(p.KEY_NAMES), len(p.TILE_STYLES), len(FAKE))
    at = 116
    names = [blob[at + 32 * i:at + 32 * i + 32].split(b'\0')[0].decode() for i in range(nkeys)]
    assert names == p.KEY_NAMES
    at += 32 * nkeys
    sizes = []
    for _ in range(nstyles):
        size, blocks = struct.unpack_from('<HH', blob, at)
        assert size == blocks * 16
        sizes.append(size)
        at += 4
    at += sum(sizes) * nkeys
    files = []
    for _ in range(nfiles):
        files.append(blob[at:at + 256].split(b'\0')[0].decode())
        at += 256
    assert files == [path.lstrip('/') for path in FAKE]
    cells = 0
    for _ in range(npatches):
        fi, n, action, style = struct.unpack_from('<HHBB', blob, at)
        at += 8
        assert fi < nfiles and action < 16 and style < nstyles and n == sizes[style] // 16
        offs = struct.unpack_from(f'<{n}I', blob, at)
        at += 4 * n
        size = max(t.w * t.h for t in FAKE['/' + files[fi]])
        assert all(o + 16 <= size for o in offs), f'{files[fi]}: patch outside the layout'
        cells += 1
    assert at == len(blob), 'the table has no trailing bytes'
    assert cells == 50, 'every replaceable cell of the seven layouts is listed'
    print(f'prompts keys: {nkeys} keys, {nstyles} tile styles, {npatches} patches, {len(blob)} bytes')


if __name__ == '__main__':
    main()
