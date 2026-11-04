"""tools/xc2_prompts.py press_text on a made-up BDAT: the lines that mean a stick click move to the click's tag, every
other line and table stays, and the file still reads. No game files."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from xc2_prompts import bdat_cipher, press_text, table_press_tags   # noqa: E402

KEY = 0xa478
ROWS_AT, ROW_SIZE = 0x30, 8
L3, R3, A = '[ML:icon icon=L3 ]', '[ML:icon icon=R3 ]', '[ML:icon icon=A ]'
CAMERA = [f'map {L3}', f'point {R3}', f'press {R3}', f'zoom {R3}', f'mini {L3}', f'talk {A}']


def table(lines):
    """A table of one string column, at +4 of the row."""
    at = ROWS_AT + ROW_SIZE * len(lines)
    text, offsets = bytearray(), []
    for line in lines:
        offsets.append(at + len(text))
        text += line.encode() + b'\0'
    text += bytes(-len(text) % 2)
    out = bytearray(at)
    out[:4] = b'BDAT'
    struct.pack_into('<H', out, 0x08, ROW_SIZE)
    struct.pack_into('<HH', out, 0x0e, ROWS_AT, len(lines))
    struct.pack_into('<H', out, 0x16, KEY)
    struct.pack_into('<II', out, 0x18, at, len(text))
    for r, offset in enumerate(offsets):
        struct.pack_into('<I', out, ROWS_AT + r * ROW_SIZE + 4, offset)
    return bytes(out) + bdat_cipher(text, KEY, False)


def file_of(tables):
    head = 8 + 4 * len(tables)
    head += -head % 8
    out, offsets = bytearray(head), []
    for t in tables:
        offsets.append(len(out))
        out += t + bytes(-len(t) % 8)
    struct.pack_into('<II', out, 0, len(tables), len(out))
    struct.pack_into(f'<{len(tables)}I', out, 8, *offsets)
    return bytes(out)


def lines_of(t):
    """The rows' strings, by their pointers."""
    rows, = struct.unpack_from('<H', t, 0x10)
    at, length = struct.unpack_from('<II', t, 0x18)
    text = bdat_cipher(t[at:at + length], KEY, True)
    out = []
    for r in range(rows):
        p = struct.unpack_from('<I', t, ROWS_AT + r * ROW_SIZE + 4)[0] - at
        assert 0 <= p < length, f'row {r} points outside the strings'
        out.append(text[p:text.index(b'\0', p)].decode())
    return out


def tables_of(blob):
    count, size = struct.unpack_from('<II', blob, 0)
    assert size == len(blob), 'the header size is the file size'
    offsets = struct.unpack_from(f'<{count}I', blob, 8)
    assert all(o % 8 == 0 for o in offsets), 'tables stay aligned'
    return [blob[o:offsets[k + 1] if k + 1 < count else len(blob)] for k, o in enumerate(offsets)]


def main():
    assert bdat_cipher(bdat_cipher(b'abcdef', KEY, False), KEY, True) == b'abcdef'

    want = [f'map {L3}'.replace('L3', 'L3_push'), f'point {R3}', f'press {R3}'.replace('R3', 'R3_push'), f'zoom {R3}',
            f'mini {L3}'.replace('L3', 'L3_push'), f'talk {A}']
    assert lines_of(table_press_tags(table(CAMERA))) == want

    # a table after the changed one moves and reads the same; one before it stays
    other = table([f'talk {A}', 'plain'])
    before, changed, after = tables_of(press_text(file_of([other, table(CAMERA), other])))
    assert before.rstrip(b'\0') == other.rstrip(b'\0')
    assert lines_of(changed) == want
    assert lines_of(after) == lines_of(other)

    # nothing to change, and a table of another shape: no file, so the game's own text stays
    assert press_text(file_of([other])) is None
    assert press_text(file_of([table(CAMERA[:3])])) is None
    print('prompts text: ok')


if __name__ == '__main__':
    main()
