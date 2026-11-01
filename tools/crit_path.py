"""Critical path of each frame from a XCX_CRIT recording (runtime/kernel.c crit_frame).

Walks back from every present on the presenting thread to the previous one. While the thread runs, waits for a
virtual core, sits in a host call or sleeps, the time goes to that thread and state. When it waits on a guest object,
the wake that ended the wait names the waker: the time from that signal to the end of the wait is wake latency, and
the walk goes on on the waker at the moment it signalled. A wait no guest thread ended (a timeout, a host signal)
counts whole. The sums per frame add up to the frame interval.

Usage: crit_path.py <crit.bin> [--top N]
"""
import argparse
import bisect
import struct
from collections import Counter, defaultdict

import numpy as np

STATES = ['off', 'run', 'core', 'host', 'sleep', 'sync', 'cv', 'lock', 'addr']
S_RUN, S_CORE, S_HOST, S_SLEEP = 1, 2, 3, 4
CRIT_STATE, CRIT_WAKE, CRIT_FRAME = 0, 1, 2
EV = np.dtype([('ts', '<u8'), ('obj', '<u8'), ('a', '<u4'), ('b', '<u4'), ('kind', '<u2'), ('st', '<u2'),
               ('pad', '<u4')])


def load(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'CRIT', path
    nt, ne = struct.unpack_from('<II', data, 4)
    threads = {}
    for i in range(nt):
        h, prio, flags, tid = struct.unpack_from('<IiII', data, 12 + 16 * i)
        threads[h] = (prio, flags, tid)
    ev = np.frombuffer(data, EV, ne, 12 + 16 * nt)
    return threads, ev


def label(h, threads):
    prio, flags, _ = threads.get(h, (0, 0, 0))
    role = 'main' if flags & 1 else 'present' if flags & 2 else ''
    return f'{h:#x} p{prio}{" " + role if role else ""}' if h else 'host'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('crit')
    ap.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    threads, ev = load(a.crit)
    seg = defaultdict(lambda: ([], [], []))   # per thread: starts, states, wakers at the start
    wakes = defaultdict(lambda: ([], []))     # per thread: signal times, wakers
    frames = []
    for e in ev:
        k = int(e['kind'])
        if k == CRIT_STATE:
            s = seg[int(e['a'])]
            s[0].append(int(e['ts']))
            s[1].append(int(e['st']))
            s[2].append(int(e['b']))
        elif k == CRIT_WAKE:
            w = wakes[int(e['a'])]
            w[0].append(int(e['ts']))
            w[1].append(int(e['b']))
        else:
            frames.append((int(e['ts']), int(e['a']), int(e['obj'])))
    tot = Counter()
    lens = []
    for (t0, p0, _), (t1, p1, _) in zip(frames, frames[1:]):
        x, t, steps = p1, t1, 0
        acc = Counter()
        while t > t0 and steps < 100000:
            steps += 1
            starts, states, _ = seg[x]
            i = bisect.bisect_left(starts, t) - 1
            if i < 0:
                acc[(x, 'unknown')] += t - t0
                break
            st, lo = states[i], max(starts[i], t0)
            if st in (S_RUN, S_CORE, S_HOST, S_SLEEP) or st >= len(STATES):
                acc[(x, STATES[st] if st < len(STATES) else f's{st}')] += t - lo
                t = lo
                continue
            if st == 0:
                acc[(x, 'off')] += t - lo
                t = lo
                continue
            # a wait from starts[i] to the next state change: the last signal for x inside it ended it
            end = starts[i + 1] if i + 1 < len(starts) else t
            ws, wk = wakes[x]
            j = bisect.bisect_right(ws, end) - 1
            if j >= 0 and ws[j] >= starts[i] and wk[j]:
                w = ws[j]
                if w <= t0:
                    acc[(x, 'wake latency')] += t - t0
                    break
                acc[(x, 'wake latency')] += max(0, t - w)
                x, t = wk[j], min(t, w)
            else:
                acc[(x, 'wait ' + STATES[st] + ' (no guest waker)')] += t - lo
                t = lo
        tot.update(acc)
        lens.append(t1 - t0)
    n = len(lens)
    if not n:
        raise SystemExit('fewer than two presents recorded')
    frame_ms = sum(lens) / n / 1e6
    print(f'{n} frames, mean interval {frame_ms:.2f} ms; critical path per frame (ms, share):')
    for (h, kind), ns in tot.most_common(a.top):
        print(f'  {ns / n / 1e6:7.3f}  {100 * ns / sum(lens):5.1f}%  {label(h, threads):24} {kind}')
    by_kind = Counter()
    for (h, kind), ns in tot.items():
        by_kind[kind] += ns
    print('by kind: ' + ', '.join(f'{k} {v / n / 1e6:.2f}' for k, v in by_kind.most_common()))
    by_thread = Counter()
    for (h, kind), ns in tot.items():
        by_thread[h] += ns
    print('by thread: ' + ', '.join(f'{label(h, threads)} {v / n / 1e6:.2f}' for h, v in by_thread.most_common(10)))


if __name__ == '__main__':
    main()
