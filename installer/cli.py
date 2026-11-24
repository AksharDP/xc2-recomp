"""Command line: python -m installer --base FILE --update FILE --keys FILE [--dlc FILE ...] --lang CODE --dest DIR
                                     [--root DIR] [--temp DIR] [--verify] [--progress-lines]

A first install needs every option but --lang, which defaults to en-US, or Japanese when the game files have no
English (the Japanese release). The language must be one the game files have text for: the Americas and Europe release
has English, French, German, Italian, Spanish and Korean, the Japanese one Japanese and Chinese (python -m
installer.probe lists them). On a folder with xc2-install.json (resume after a cancel or failure, or update with a
newer release) only --dest is needed: --lang defaults to the recorded language, and the game files are needed only
when the extraction has to run again. Prints JSON events on stdout (events.py). Exit code: 0 done, 1 failed,
2 bad command line, 3 cancelled (Ctrl+C or Ctrl+Break).
"""
import argparse
import signal
import sys
import time
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from .events import LauncherReporter, Reporter
from .runner import Context, run_steps
from .steps import adopt_old_temp, all_steps

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error('cli.usage', {'detail': message})
        sys.exit(2)


def cancel(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    ap = Parser(prog='python -m installer', description=__doc__.splitlines()[0])
    ap.add_argument('--base', type=Path, help='base game, XCI or NSP')
    ap.add_argument('--update', type=Path, help='3.0.1 update NSP')
    # extend: RecompLauncher passes one --dlc=FILE per add-on it was given; the plain form takes them all in one option
    ap.add_argument('--dlc', type=Path, nargs='*', action='extend', help='add-on content NSPs (optional)')
    ap.add_argument('--keys', type=Path, help='prod.keys')
    ap.add_argument('--lang', help='game language, a Switch language code (en-US, en-GB, ja, de, fr, ...)')
    ap.add_argument('--dest', type=Path, required=True, help='install folder')
    ap.add_argument('--root', type=Path, help='release folder or repository (default: the folder above installer/)')
    ap.add_argument('--temp', type=Path, help='temp folder (default: build-cache/work in the install folder on Windows, '
                                              '.setup-temp in it on Linux, build/install-temp in a repository)')
    ap.add_argument('--verify', action='store_true', help='hash every kept file, also those this release made (an update otherwise keeps the RomFS by its '
                              'recorded size and time)')
    ap.add_argument('--progress-lines', action='store_true', help='print "progress <0 to 1> <text>" lines instead of JSON '
                                                                  'events (RecompLauncher)')
    args = ap.parse_args(argv)

    # The window cancels with Ctrl+Break to the installer's process group (CREATE_NEW_PROCESS_GROUP), which also reaches
    # the tools it runs; Python's default for it ends the process without recording anything.
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    layout = layouts.find(args.root)
    dest = args.dest.resolve()
    temp = layouts.temp_for(layout, dest, args.temp)
    adopted = adopt_old_temp(layout, dest, temp)
    log = temp / 'logs' / time.strftime('install-%Y%m%d-%H%M%S.log')
    rep = (LauncherReporter if args.progress_lines else Reporter)(log_path=log)
    if adopted:
        rep.log(adopted)
    t0 = time.monotonic()
    try:
        state = statefile.load(dest)
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        rep.emit('end', result='failed', seconds=0)
        return 1
    # Without --lang (the launcher has no language field) a first install gets the default of the game's languages
    # (en-US, or Japanese on the Japanese release: installer/steps.py Check.language), and an update the recorded one
    ctx = Context(layout, dest, temp, rep, args, state=state, verify=args.verify)
    steps = all_steps()
    rep.emit('start', release=layout.release, mode=ctx.mode, dev=layout.dev, dest=str(dest), log=str(log),
             steps=[s.id for s in steps])
    result = run_steps(steps, ctx)
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]
