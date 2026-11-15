"""Read XC2_FLIGHT_SLOW_MS logs (runtime/flight.c, slow.tsv): what the frames over the threshold have in common.

    python tools/flight_slow.py <slow.tsv or a folder holding them> [...] [--from-world S | --from-t S] [--rows N]

For each log, and pooled over all of them: the frames' interval spread, the mean of every timing column, the CPU time
of each watched thread, the busiest other threads, and the counts that differ most from the log's median frame. Each
frame is also given the thread whose CPU time comes nearest its interval (the likely critical path), and the share of
frames per such thread is listed. --rows prints the N slowest frames with their main columns.
"""
import argparse
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

TIME = ['guest_ms', 'capture_ms', 'drain_frame_ms', 'drain_full_ms', 'drain_other_ms', 'pace_ms', 'fence_ms', 'drain_ms',
        'pipe_ms', 'xlate_ms', 'tex_ms', 'astc_ms', 'pres_fence_ms', 'pres_acq_ms', 'gpu_ms', 'replay_ms']
CPU = ['cpu_pres_ms', 'cpu_main_ms', 'cpu_replay_ms', 'cpu_vkpres_ms', 'cpu_copy_ms', 'cpu_guest_other_ms',
       'cpu_host_other_ms']
COUNTS = ['submits', 'passes', 'draws', 'tex_up', 'tex_new', 'pipe_built', 'pipe_waited', 'shaders_xlated', 'mirror_kb',
          'stage_kb', 'wb_done']


def load(path):
    lines = path.read_text(encoding='utf-8').splitlines()
    rows = [ln.split('\t') for ln in lines if ln and not ln.startswith('#')]
    cols = rows[0]
    out = []
    for r in rows[1:]:
        d = {}
        for c, v in zip(cols, r):
            try:
                d[c] = float(v)
            except ValueError:
                d[c] = v
        out.append(d)
    return lines[0] if lines and lines[0].startswith('#') else '', out


def mean(xs):
    xs = [x for x in xs if isinstance(x, float) and not math.isnan(x)]
    return sum(xs) / len(xs) if xs else float('nan')


def limit(f):
    """The single thread whose CPU time is the largest share of the interval, and that share (the summed columns of
    the other guest and host threads and the copy helpers are not single threads)."""
    best, share = 'none', 0.0
    for c in CPU[:4]:
        v = f.get(c)
        if isinstance(v, float) and v / f['interval_ms'] > share:
            best, share = c[4:-3], v / f['interval_ms']
    for j in (1, 2, 3):
        v = f.get(f'top{j}_ms')
        if isinstance(v, float) and v / f['interval_ms'] > share:
            best, share = str(f[f'top{j}']), v / f['interval_ms']
    return best, share


def report(name, frames, rows):
    if not frames:
        print(f'{name}: no frames')
        return
    iv = sorted(f['interval_ms'] for f in frames)
    print(f'== {name}: {len(frames)} frames, interval median {statistics.median(iv):.1f} ms, '
          f'p90 {iv[int(0.9 * (len(iv) - 1))]:.1f}, max {iv[-1]:.1f}')
    buckets = Counter('20-25' if x < 25 else '25-33' if x < 33.4 else '33-50' if x < 50 else '50-100' if x < 100
                      else '100+' for x in iv)
    print('   intervals: ' + ', '.join(f'{k} ms {buckets[k]}' for k in ['20-25', '25-33', '33-50', '50-100', '100+']))
    have = frames[0].keys()
    print('   mean ms: interval %.2f' % mean([f['interval_ms'] for f in frames]) + ''.join(
        f', {c[:-3]} {mean([f[c] for f in frames]):.2f}' for c in TIME if c in have))
    if 'cpu_pres_ms' in have:
        print('   cpu ms:  ' + ', '.join(f'{c[4:-3]} {mean([f[c] for f in frames]):.2f}' for c in CPU))
        tops = defaultdict(float)
        for f in frames:
            for j in (1, 2, 3):
                if f.get(f'top{j}') not in (None, '-'):
                    tops[str(f[f'top{j}'])] += f[f'top{j}_ms']
        print('   other threads, cpu ms per slow frame: ' + ', '.join(
            f'{k} {v / len(frames):.2f}' for k, v in sorted(tops.items(), key=lambda kv: -kv[1])[:8]))
        lim = Counter()
        for f in frames:
            lim[limit(f)[0]] += 1
        print('   busiest thread (largest CPU share of the interval): ' + ', '.join(
            f'{k} {100 * v / len(frames):.0f}%' for k, v in lim.most_common(6)))
        print('   mean CPU share of the interval: pres %.0f%%, main %.0f%%, replay %.0f%%' % tuple(
            100 * mean([f[c] / f['interval_ms'] for f in frames]) for c in CPU[:3]))
    print('   mean counts: ' + ', '.join(f'{c} {mean([f[c] for f in frames]):.1f}' for c in COUNTS if c in have))
    if rows:
        print('   slowest frames:')
        print('     frame   world_s  ms     guest  capt  drainF drainO pres   main   replay  gpu    stageMB  tex_up '
              'draws  top1')
        for f in sorted(frames, key=lambda f: -f['interval_ms'])[:rows]:
            print(f"     {int(f['frame']):6d}  {f.get('world_ms', -1) / 1000:6.2f}  {f['interval_ms']:6.1f} "
                  f"{f['guest_ms']:6.1f} {f['capture_ms']:5.1f} {f['drain_frame_ms']:6.1f} {f['drain_other_ms']:6.1f} "
                  f"{f.get('cpu_pres_ms', float('nan')):6.1f} {f.get('cpu_main_ms', float('nan')):6.1f} "
                  f"{f.get('cpu_replay_ms', float('nan')):6.1f}  {f['gpu_ms']:6.1f} {f['stage_kb'] / 1024:7.1f}  "
                  f"{int(f['tex_up']):6d} {int(f['draws']):5d}  {f.get('top1', '-')} {f.get('top1_ms', 0):.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+', type=Path)
    ap.add_argument('--from-world', type=float, help='only frames at or after this world-clock second')
    ap.add_argument('--from-t', type=float, help='only frames at or after this second of the flight clock')
    ap.add_argument('--rows', type=int, default=0)
    ap.add_argument('--split', type=float, nargs=2, metavar=('FAST', 'SLOW'),
                    help='for a log taken with a low threshold: report frames under FAST ms and over SLOW ms apart')
    a = ap.parse_args()
    files = []
    for p in a.paths:
        files += sorted(p.rglob('slow.tsv')) if p.is_dir() else [p]
    if not files:
        sys.exit('no slow.tsv found')
    pooled = []
    for p in files:
        head, frames = load(p)
        if a.from_world is not None:
            frames = [f for f in frames if f.get('world_ms', -1) >= a.from_world * 1000]
        if a.from_t is not None:
            frames = [f for f in frames if f['t_ms'] >= a.from_t * 1000]
        print(head)
        if a.split:
            n = len(frames)
            fast = [f for f in frames if f['interval_ms'] < a.split[0]]
            slow = [f for f in frames if f['interval_ms'] > a.split[1]]
            print(f'   {n} frames: {len(fast)} under {a.split[0]} ms, {len(slow)} over {a.split[1]} ms '
                  f'({100 * len(slow) / max(n, 1):.1f} %)')
            if len(files) == 1:
                report(f'under {a.split[0]} ms', fast, 0)
                report(f'over {a.split[1]} ms', slow, a.rows)
            pooled += frames
            continue
        report(str(p), frames, a.rows)
        pooled += frames
    if a.split and len(files) > 1:
        fast = [f for f in pooled if f['interval_ms'] < a.split[0]]
        slow = [f for f in pooled if f['interval_ms'] > a.split[1]]
        report(f'pooled, under {a.split[0]} ms', fast, 0)
        report(f'pooled, over {a.split[1]} ms', slow, a.rows)
    elif len(files) > 1:
        report(f'pooled over {len(files)} logs', pooled, a.rows)


if __name__ == '__main__':
    main()
