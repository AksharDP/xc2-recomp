# Adapted from nxnative dm/tools/tests/test_spin_waits.py @ 6d34984 (MIT, Copyright (c) 2026 seedds).
"""tools/spin_waits.py on hand-assembled AArch64 loops, without game files.

Run: .venv/Scripts/python tests/spin_waits_test.py
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))

from spin_waits import Search  # noqa: E402

# A job queue poll (the shape of Breath of the Wild's main+0xef6354): take the queue's spin lock, look whether the wait
# is over, release the lock, again.
#   0x00 T:  cbz x20, X                 0x2c     str w12, [x20, #0x100]     0x50     ldr w13, [x21, #0x90]
#   0x04     str w11, [x20, #0x100]     0x30     ldrb w14, [x21, #0x98]     0x54     sub w8, w13, w25
#   0x08 L0: ldr w14, [x9]              0x34     cmp w0, #0                 0x58     add w15, w8, w25
#   0x0c     cmp w14, #1                0x38     ccmp w14, #0, #4, ne       0x5c     str w15, [x21, #0x94]
#   0x10     b.eq L0                    0x3c     ldr w25, [x21, #0x94]      0x60     cset w13, hs
#   0x14 L1: ldaxr w14, [x9]            0x40     ccmp w25, #0, #0, ne       0x64     b U
#   0x18     cbz w14, L2                0x44     cset w14, gt               0x68 C:  clrex
#   0x1c     clrex                      0x48     cmp w14, #1                0x6c U:  ldxr w15, [x9] / cmp w15, #1 / b.ne C
#   0x20     b L1                       0x4c     b.ne U                     0x78     stlxr w15, wzr, [x9] / cbnz w15, U
#   0x24 L2: stxr w14, w10, [x9] / cbnz w14, L1                             0x80     cbz w14, T          0x84 X: ret
# 0x50-0x64: the wait is over, take an item, store the new cursor, release and leave.
POLL = [0xb4000434, 0xb901028b, 0xb940012e, 0x710005df, 0x54ffffc0, 0x885ffd2e, 0x3400006e, 0xd5033f5f, 0x17fffffd,
        0x880e7d2a, 0x35ffff6e, 0xb901028c, 0x394262ae, 0x7100001f, 0x7a4019c4, 0xb94096b9, 0x7a401b20, 0x1a9fd7ee,
        0x710005df, 0x54000101, 0xb94092ad, 0x4b1901a8, 0x0b19010f, 0xb90096af, 0x1a9f37ed, 0x14000002, 0xd5033f5f,
        0x885f7d2f, 0x710005ff, 0x54ffffa1, 0x880ffd3f, 0x35ffff8f, 0x34fffc0e, 0xd65f03c0]


def region(words, T, bes):
    img = struct.pack('<' + 'I' * len(words), *words) + bytes(64)
    return Search(img, 4 * len(words), set()).region(T, bes)


def patched(**at):
    w = list(POLL)
    for off, word in at.items():
        w[int(off[1:], 16) // 4] = word
    return w


class SpinWaits(unittest.TestCase):
    def test_lock_poll_is_a_wait(self):
        g = region(POLL, 0, [0x80])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {0x80})
        self.assertNotIn(0x5c, g['R'])   # the path that takes an item stores a new cursor: not part of the wait
        self.assertTrue(set(g['kinds']) >= {'acq', 'rel', 'tst', 'stc'})

    def test_found_by_the_scan(self):
        self.assertIn(0, {g['T'] for g in Search(struct.pack('<' + 'I' * len(POLL), *POLL), 4 * len(POLL), set()).all()})

    def test_patch_site_inside(self):   # a site of the patch table in the region keeps it out
        img = struct.pack('<' + 'I' * len(POLL), *POLL) + bytes(64)
        self.assertIsNone(Search(img, 4 * len(POLL), {0x3c}).region(0, [0x80]))

    def test_store_of_a_changing_value(self):   # the wait path stores w14, which the pass writes
        self.assertIsNone(region(patched(o2c=0xb901028e), 0, [0x80]))

    def test_lock_not_released(self):   # the wait path skips the release (b.ne U -> b.ne to the back-edge)
        self.assertIsNone(region(patched(o4c=0x540001a1), 0, [0x80]))

    def test_nothing_polled(self):   # str xzr, [x8]; b back: stores of a constant and no load
        self.assertIsNone(region([0xf900011f, 0x17ffffff], 0, [4]))

    def test_plain_test_loop(self):   # ldr w8, [x0]; cbz w8, back
        g = region([0xb9400008, 0x34ffffe8], 0, [4])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {4})

    def test_counted_loop(self):   # ldr w8, [x0]; subs w1, w1, #1; b.ne back: a counter, it leaves by itself
        self.assertIsNone(region([0xb9400008, 0x71000421, 0x54ffffc1], 0, [8]))


if __name__ == '__main__':
    unittest.main()
