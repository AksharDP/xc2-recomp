"""Read a XCX_FLIGHT dump (runtime/flight.c): the frames around the worst interval, the frames whose counts stand out,
and a contact sheet of the thumbnails with frame number and interval on each.

    python tools/flight_view.py build/flight/<YYYYMMDD-HHMMSS>-f<frame> [--around FRAME] [--span 48] [--cols 12]

The sheet goes to <dump>/contact.png (--out to change): --span frames each side of the trigger (or of --around),
the worst frame boxed red, frames over 1.5x the median interval orange.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

from PIL import Image, ImageDraw

BLOCKED = ['pace_ms', 'fence_ms', 'drain_ms', 'pipe_ms', 'xlate_ms', 'tex_ms', 'astc_ms']
# Counts that are 0 in a steady frame: any non-zero value is listed.
RARE = ['shaders_xlated', 'pipe_built', 'pipe_waited', 'oneshot_waited', 'pipe_bg_used', 'draws_skip_pipe',
        'draws_skip_shader', 'tex_new', 'tex_alias', 'tex_up', 'tex_reup', 'wb_done', 'wb_skip']
# Counts with a steady level: frames off the median by more than a quarter are listed.
LEVEL = ['submits', 'passes', 'draws', 'draws_seen', 'clears', 'copies', 'dispatches']
SHORT = {'interval_ms': 'ms', 'pace_ms': 'pace', 'fence_ms': 'fence', 'drain_ms': 'drain', 'pipe_ms': 'pipe',
         'xlate_ms': 'xlate', 'tex_ms': 'tex', 'astc_ms': 'astc'}


def load(dump):
    lines = (dump / 'frames.tsv').read_text(encoding='utf-8').splitlines()
    head = lines[0] if lines and lines[0].startswith('#') else ''
    rows = [ln.split('\t') for ln in lines if ln and not ln.startswith('#')]
    cols = rows[0]
    frames = []
    for r in rows[1:]:
        d = {c: float(v) for c, v in zip(cols, r)}
        d['frame'] = int(d['frame'])
        frames.append(d)
    return head, cols, frames


def describe(f, cols):
    """One line per frame: interval, blocked times over 1 ms, then every non-zero rare count."""
    parts = [f"f{f['frame']:<6} {f['interval_ms']:7.1f} ms"]
    blocked = [f"{SHORT[c]} {f[c]:.1f}" for c in BLOCKED if f.get(c, 0) >= 1]
    parts.append(('blocked: ' + ' '.join(blocked)) if blocked else '')
    parts.append(f"draws {int(f['draws'])} passes {int(f['passes'])}")
    rare = [f"{c} {int(f[c])}" for c in RARE if c in cols and f[c]]
    parts.append(' '.join(rare))
    return '  '.join(p for p in parts if p)


def deviations(frames, cols):
    out = []
    for c in RARE:
        if c not in cols:
            continue
        hits = [(f['frame'], int(f[c])) for f in frames if f[c]]
        if hits:
            total = sum(v for _, v in hits)
            shown = ' '.join(f'f{fr}={v}' for fr, v in sorted(hits, key=lambda h: -h[1])[:10])
            out.append(f'  {c:18} {len(hits):4} frames, total {total:6}: {shown}{" ..." if len(hits) > 10 else ""}')
    for c in LEVEL:
        vals = [f[c] for f in frames]
        med = statistics.median(vals)
        tol = max(2.0, 0.25 * med)
        hits = [(f['frame'], int(f[c])) for f in frames if abs(f[c] - med) > tol]
        if hits:
            shown = ' '.join(f'f{fr}={v}' for fr, v in sorted(hits, key=lambda h: -abs(h[1] - med))[:10])
            out.append(f'  {c:18} {len(hits):4} frames off median {med:g}: {shown}{" ..." if len(hits) > 10 else ""}')
    return out


def sheet(dump, frames, centre, span, cols_n, out, worst, slow):
    sel = [f for f in frames if abs(f['frame'] - centre) <= span]
    tiles = [(f, dump / f"f{f['frame']:06d}.png") for f in sel]
    tiles = [(f, p) for f, p in tiles if p.exists()]
    if not tiles:
        return 0
    tw, th, label = 240, 135, 16
    rows = (len(tiles) + cols_n - 1) // cols_n
    img = Image.new('RGB', (cols_n * tw, rows * (th + label)), (24, 24, 24))
    draw = ImageDraw.Draw(img)
    for i, (f, p) in enumerate(tiles):
        x, y = (i % cols_n) * tw, (i // cols_n) * (th + label)
        img.paste(Image.open(p).convert('RGB').resize((tw, th), Image.BILINEAR), (x, y + label))
        colour = (255, 64, 64) if f['frame'] == worst else (255, 170, 0) if f['interval_ms'] > slow else (220, 220, 220)
        draw.text((x + 3, y + 2), f"f{f['frame']}  {f['interval_ms']:.1f} ms", fill=colour)
        if colour != (220, 220, 220):
            draw.rectangle([x, y + label, x + tw - 1, y + label + th - 1], outline=colour, width=3)
    img.save(out)
    return len(tiles)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', type=Path)
    ap.add_argument('--around', type=int, help='centre of the sheet and a second listing (default: the trigger)')
    ap.add_argument('--span', type=int, default=48, help='frames each side on the contact sheet (default 48)')
    ap.add_argument('--list', type=int, default=8, help='frames each side in the listing (default 8)')
    ap.add_argument('--cols', type=int, default=12, help='contact sheet columns')
    ap.add_argument('--out', type=Path, help='contact sheet path (default <dump>/contact.png)')
    a = ap.parse_args()
    if not (a.dump / 'frames.tsv').exists():
        sys.exit(f'{a.dump}: no frames.tsv')
    head, cols, frames = load(a.dump)
    if not frames:
        sys.exit(f'{a.dump}: no frames')
    iv = sorted(f['interval_ms'] for f in frames[1:] or frames)
    med = statistics.median(iv)
    worst = max(frames[1:] or frames, key=lambda f: f['interval_ms'])
    print(head.lstrip('# ') or a.dump.name)
    print(f"frames {frames[0]['frame']}-{frames[-1]['frame']} ({len(frames)}), interval median {med:.1f} ms, "
          f"p95 {iv[int(0.95 * (len(iv) - 1))]:.1f} ms, max {worst['interval_ms']:.1f} ms at f{worst['frame']}; "
          f"thumbnails {sum(int(f.get('thumb', 0)) for f in frames)}")
    m = re.search(r'at frame (\d+)', head)
    centre = a.around if a.around is not None else int(m[1]) if m else worst['frame']
    for c in dict.fromkeys([worst['frame'], centre]):
        what = 'worst interval' if c == worst['frame'] else 'trigger' if a.around is None else '--around'
        print(f'\nframes around f{c} ({what}):')
        for f in frames:
            if abs(f['frame'] - c) <= a.list:
                print(('> ' if f['frame'] == c else '  ') + describe(f, cols))
    slow = 1.5 * med
    spikes = [f for f in frames if f['interval_ms'] > slow]
    print(f'\nslow frames (> {slow:.1f} ms): {len(spikes)}')
    for f in sorted(spikes, key=lambda f: -f['interval_ms'])[:12]:
        print('  ' + describe(f, cols))
    dev = deviations(frames, cols)
    print('\ncounts that stand out:' if dev else '\ncounts that stand out: none')
    print('\n'.join(dev))
    out = a.out or a.dump / 'contact.png'
    n = sheet(a.dump, frames, centre, a.span, a.cols, out, worst['frame'], slow)
    print(f'\ncontact sheet: {out} ({n} thumbnails)' if n else '\ncontact sheet: no thumbnails in range')


if __name__ == '__main__':
    main()
