"""Check the catalog of the settings menu and the overlay (runtime/ui_text.c, .h). No game files, standard library only:
CI runs it on every push and pull request.

Usage: ui_text_test.py [--strict]

Errors (exit 1): an id without English text, two entries for one id in a language, and a translation whose printf
conversions differ from the English ones (menu_text strings are used as formats, so that reads a wrong argument).
A menu id without a Chinese text is a warning, as the menu then shows the English one; --strict makes it an error.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path('C:/msys64/ucrt64/bin')
CATALOG = 'runtime/ui_text.c'
MENU_LANGS = ('zh-Hans', 'zh-Hant')
# Not ids of their own: another name for an id, and the count.
NOT_IDS = {'UI_MENU_FIRST', 'UI_MENU_LAST_ID', 'UI_TEXT_COUNT'}
# No blank flag: "100 % default" in a description is not a conversion.
CONVERSION = re.compile(r'%(?:%|[-+0#]*\d*(?:\.\d+)?(hh|h|ll|l|z|j|t)?([diouxXeEfFgGcsp]))')


def enum_ids(header):
    """The ids of UiTextId, in order."""
    body = re.search(r'typedef enum \{(.*?)\} UiTextId;', header, re.S).group(1)
    body = re.sub(r'//[^\n]*|/\*.*?\*/', '', body, flags=re.S)
    names = [part.split('=')[0].strip() for part in body.split(',')]
    return [n for n in names if n and n not in NOT_IDS]


def conversions(text):
    """The arguments a printf format reads: [('', 'f'), ('', 's')]. Width and precision may differ in a translation."""
    return [m.groups() for m in CONVERSION.finditer(text) if m.group(2)]


def check(names, text):
    """Errors and warnings as lists of messages. text: {(id, language): its own text, '' for none}."""
    errors, warnings = [], []
    langs = sorted({lang for _, lang in text})
    for name in names:
        en = text[name, 'en']
        if not en:
            errors.append(f'{name} has no English text')
            continue
        for lang in langs:
            other = text[name, lang]
            if other and conversions(other) != conversions(en):
                errors.append(f'{name}: the conversions in {lang} ({other!r}) differ from the English ones ({en!r})')
    for lang in MENU_LANGS:
        missing = [n for n in names if n.startswith(('UI_MENU_', 'UI_PERF_')) and not text[n, lang]]
        if missing:
            warnings.append(f'no {lang} text, so the menu shows English: {", ".join(missing)}')
    return errors, warnings


def compiler():
    # on Windows MSYS2's, which the game is built with: the clang on PATH may be the MSVC one, without <strings.h>
    for cc in (os.environ.get('CC'), str(MSYS_BIN / 'clang.exe') if os.name == 'nt' else None, 'clang'):
        if cc and shutil.which(cc):
            return shutil.which(cc)
    sys.exit('ui_text_test: clang not found (set CC)')


def dump(names):
    """The catalog as the compiler built it."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'ui_text_ids.h').write_text(''.join(f'{{{n}, "{n}"}},\n' for n in names), encoding='utf-8')
        exe = tmp / ('dump.exe' if os.name == 'nt' else 'dump')
        cc = compiler()
        env = dict(os.environ, PATH=str(Path(cc).parent) + os.pathsep + os.environ.get('PATH', ''))
        subprocess.run([cc, '-O0', '-march=x86-64-v3', '-Wall', '-Werror=initializer-overrides',
                        '-Werror=implicit-function-declaration', '-I', str(ROOT / 'runtime'), '-I', str(tmp),
                        str(ROOT / 'tests' / 'ui_text_dump.c'), '-o', str(exe)], check=True, env=env)
        out = subprocess.run([str(exe)], check=True, capture_output=True, env=env).stdout.decode('utf-8')
    return {(name, lang): text for name, lang, text in (r.split('\x1f') for r in out.split('\x1e') if r)}


def self_test():
    """The rules themselves, on a made-up catalog."""
    def one(**texts):
        return check(['UI_MENU_X'], {('UI_MENU_X', k.replace('_', '-')): v for k, v in texts.items()})
    good = dict(en='%.1f of %s, 100 % default', zh_Hans='%s'.join(['%.0f ', '']), zh_Hant='%f %s')
    assert one(**good) == ([], []), one(**good)
    assert len(one(**{**good, 'zh_Hans': '%s %f'})[0]) == 1           # another order
    assert len(one(**{**good, 'zh_Hant': '%f'})[0]) == 1              # one lost
    assert len(one(**{**good, 'en': ''})[0]) == 1                     # no English
    assert one(**{**good, 'zh_Hans': ''})[0] == [] and len(one(**{**good, 'zh_Hans': ''})[1]) == 1
    assert conversions('%ux%u, %.0f%% %lld') == [(None, 'u'), (None, 'u'), (None, 'f'), ('ll', 'd')]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--strict', action='store_true', help='a missing Chinese text is an error too')
    args = ap.parse_args()
    self_test()
    names = enum_ids((ROOT / 'runtime' / 'ui_text.h').read_text(encoding='utf-8'))
    errors, warnings = check(names, dump(names))
    if args.strict:
        errors, warnings = errors + warnings, []
    github = os.environ.get('GITHUB_ACTIONS') == 'true'   # shown on the pull request
    for kind, messages in (('error', errors), ('warning', warnings)):
        for m in messages:
            print(f'::{kind} file={CATALOG}::{m}' if github else f'{kind}: {m}')
    print(f'ui_text: {len(names)} ids, {len(errors)} errors, {len(warnings)} warnings')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main())
