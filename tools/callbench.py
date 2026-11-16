"""Time runtime functions offline on calls captured in the game (XC2_CALLCAP, runtime/callcap.h).

    python tools/callbench.py CAPTURES [--runtime DIR | --rev REV] [--reps N] [--kind NAME] [--csv FILE]
    python tools/callbench.py CAPTURES --base REV_OR_DIR --cand REV_OR_DIR [--rounds N]

CAPTURES is a folder of .ccap files written by a run with XC2_CALLCAP=<folder> (or single files). The bench program
(tests/callbench/callbench.c) is compiled from a runtime folder, by default this checkout's runtime/, or from a git
revision (its runtime/ is exported under build/callbench/), and replays every captured call through that runtime's own
code: the time per call (best of --reps) and whether the result is the one the game got.

With --base and --cand both are built and run alternately --rounds times (default 4), and the median time per kind is
compared: a change to inflate_fast, nvn_deswizzle or the like is judged on the game's own data, without the game.
Captures stay under build/ or wherever XC2_CALLCAP pointed: they hold game data.
"""
import argparse
import hashlib
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402  (toolchain paths)

OUT = ROOT / 'build' / 'callbench'
SRC = ROOT / 'tests' / 'callbench' / 'callbench.c'
RUNTIME_FILES = ('tex_deswizzle.c', 'nvn_tex.c')   # compiled beside callbench.c, which includes ov_inflate.c


def runtime_dir(spec):
    """A runtime folder for spec: an existing folder (holding runtime/ or being it), or a git revision exported once."""
    p = Path(spec)
    if p.is_dir():
        return p / 'runtime' if (p / 'runtime').is_dir() else p
    sha = subprocess.run(['git', 'rev-parse', '--verify', spec + '^{commit}'], cwd=ROOT, capture_output=True,
                         text=True, check=True).stdout.strip()
    dest = OUT / f'rev-{sha[:12]}'
    if not (dest / 'runtime').is_dir():
        dest.mkdir(parents=True, exist_ok=True)
        tar = subprocess.run(['git', 'archive', sha, 'runtime'], cwd=ROOT, capture_output=True, check=True).stdout
        subprocess.run(['tar', '-x', '-C', str(dest)], input=tar, check=True)
    return dest / 'runtime'


def build(rt):
    """The bench program compiled from runtime folder rt (rebuilt when any input changed)."""
    inputs = [SRC, ROOT / 'runtime' / 'callcap.h'] + [rt / f for f in RUNTIME_FILES] + sorted(rt.glob('*.h')) + [rt / 'ov_inflate.c']
    key = hashlib.sha256(b''.join(p.read_bytes() for p in inputs if p.is_file())).hexdigest()[:16]
    exe = OUT / f'callbench-{key}{".exe" if build_game.WIN else ""}'
    if exe.is_file():
        return exe
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = [str(build_game.CLANG), '-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-DCALLBENCH', '-I', str(rt),
           '-I', str(build_game.vma_include()), str(SRC)] + [str(rt / f) for f in RUNTIME_FILES] + ['-o', str(exe)]
    tmp = OUT / 'tmp'   # clang's temporary files beside the output: the system temp folder may not be writable
    tmp.mkdir(exist_ok=True)
    env = dict(os.environ, TEMP=str(tmp), TMP=str(tmp), TMPDIR=str(tmp))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(f'callbench: build failed for {rt}:\n{r.stderr}')
    return exe


def run(exe, caps, reps, kind=None, csv=None):
    cmd = [str(exe)] + [str(c) for c in caps] + ['--reps', str(reps)]
    if kind:
        cmd += ['--kind', kind]
    if csv:
        cmd += ['--csv', str(csv)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def table(text):
    """{kind: (calls, captured ms/frame, here ms/frame, differ, skipped)} from the program's output."""
    rows = {}
    for ln in text.splitlines():
        m = re.match(r'(\w+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\d+)\s+(\d+)$', ln.strip())
        if m:
            rows[m[1]] = (int(m[2]), float(m[3]), float(m[4]), int(m[6]), int(m[7]))
    return rows


def main():
    global OUT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('captures', nargs='+', type=Path)
    ap.add_argument('--runtime', help='runtime folder or git revision to build from (default: this checkout)')
    ap.add_argument('--rev', help='same as --runtime with a git revision')
    ap.add_argument('--base', help='compare: the reference runtime folder or revision')
    ap.add_argument('--cand', help='compare: the changed runtime folder or revision')
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--kind', choices=['inflate', 'deswizzle', 'copy', 'wwquery'])
    ap.add_argument('--csv', type=Path, help='one line per call')
    ap.add_argument('--out', type=Path, help='where the bench programs and exported revisions go')
    a = ap.parse_args()
    OUT = a.out or OUT
    if bool(a.base) != bool(a.cand):
        sys.exit('callbench: --base and --cand go together')
    if not a.base:
        exe = build(runtime_dir(a.runtime or a.rev or str(ROOT)))
        rc, out = run(exe, a.captures, a.reps, a.kind, a.csv)
        print(out, end='')
        sys.exit(rc)
    exes = {'base': build(runtime_dir(a.base)), 'cand': build(runtime_dir(a.cand))}
    times = {k: {} for k in exes}
    cover = {k: {} for k in exes}   # per kind, the (calls, skipped) of each round
    differ = {k: 0 for k in exes}
    for r in range(a.rounds):
        for name in (('base', 'cand') if r % 2 == 0 else ('cand', 'base')):
            rc, out = run(exes[name], a.captures, a.reps, a.kind)
            rows = table(out)
            if rc not in (0, 1) or not rows:   # 1 reports results that differ; anything else did not run
                sys.exit(f'callbench: the {name} bench failed (status {rc}):\n{out}')
            for kind, (calls, _cap, here, d, skipped) in rows.items():
                cover[name].setdefault(kind, set()).add((calls, skipped))
                if calls:   # a kind whose calls were all skipped has no time to compare
                    times[name].setdefault(kind, []).append(here)
                differ[name] += d
            print(f'round {r + 1} {name}: ' + ', '.join(f'{k} {v[-1]:.3f}' for k, v in times[name].items()), flush=True)
    print(f'\nmedian ms per captured frame over {a.rounds} rounds (results differing from the game: base '
          f'{differ["base"]}, cand {differ["cand"]})')
    compared = 0
    for kind in sorted(set(times['base']) | set(times['cand'])):
        b, c = times['base'].get(kind), times['cand'].get(kind)
        cb, cc = cover['base'].get(kind), cover['cand'].get(kind)
        if b and c and len(b) == len(c) == a.rounds and cb == cc and len(cb) == 1:   # the same calls replayed each time
            mb, mc = statistics.median(b), statistics.median(c)
            print(f'  {kind:10} base {mb:8.3f}  cand {mc:8.3f}' + (f'  cand/base {mc / mb:.3f}' if mb else ''))
            compared += 1
        else:
            print(f'  {kind:10} not the same calls replayed in every round of both: no comparison')
    if not compared:
        sys.exit('callbench: no kind of call ran in both benches')
    sys.exit(1 if differ['cand'] else 0)


if __name__ == '__main__':
    main()
