"""xc2.ini: set the game language and keep everything else (keys and syntax as runtime/cfg.c reads them).

A new file starts from xc2.ini.example, whose lines are all commented defaults. The language goes into `lang`
(a Switch language code, runtime/ov_system.c) and the languages the installed game files have text for into
`game_langs` (runtime/ov_system.c ov_language_shipped). Each key: an active `key = ...` line is rewritten in place,
else the commented `# key = ...` line of the example is activated, else the line is added to [language] (created at
the end if missing). Other lines, comments and line order stay as they are.
"""
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
from xc2_languages import CODES  # noqa: E402

# The codes the game knows, in the order the window lists them (tools/xc2_languages.py). Which of them an install can
# use depends on its release: the Americas and Europe have English, French, German, Italian, Spanish and Korean text,
# Japan Japanese and Chinese. No release has Dutch, Portuguese or Russian (GitHub issue 107).
LANGUAGES = CODES
# Codes earlier installers wrote: zh-Hans and zh-Hant are the game's zh-CN and zh-TW; the others have no text in any
# release, so an update of an install recorded with one switches it to its release's default language.
REPLACED = {'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW'}
DROPPED_LANGUAGES = ('nl', 'pt', 'pt-BR', 'ru')
# Languages whose need for the Switch shared fonts (nn::pl, runtime/ov_system.c) is unchecked; the installer does not
# extract those fonts. Korean is not one: main of the Americas and Europe release (3.0.1) imports no nn::pl function and
# draws Korean with menu/font/standard_kr.wifnt from bf2.arh, and it runs to the opening without them. Chinese is the
# Japanese release's, whose files were not available to check; bf2.arh ships chinese_simple.wifnt and
# chinese_original.wifnt in the other release too.
FONTS_UNCHECKED = ('zh-CN', 'zh-TW')

SECTION = re.compile(r'^\s*\[(.*)\]\s*$')


def recorded_language(code):
    """A language recorded by an earlier install as this one offers it: renamed, or None when it is dropped."""
    if code in DROPPED_LANGUAGES:
        return None
    return REPLACED.get(code, code)


def default_language(languages):
    """The language an install gets when none is chosen: en-US when the game files have it, else their first."""
    return 'en-US' if 'en-US' in languages or not languages else languages[0]


def _patterns(key):
    # The key may keep the legacy xcx_ prefix of an install made before the rename to XC2 (the runtime reads it too).
    active = re.compile(rf'^(\s*)(?:xc2_|xcx_)?{key}(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
    commented = re.compile(rf'^\s*[#;]\s*(?:xc2_|xcx_)?{key}\s*=.*$', re.IGNORECASE)
    return active, commented


ACTIVE, COMMENTED = _patterns('lang')


def set_value(text, key, value):
    """text with `key = value`; see the module docstring."""
    active, commented = _patterns(key)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = active.match(line)
        if m:
            # later copies (also with the xcx_ prefix) would win at run time, so only this one stays
            lines[i + 1:] = [l for l in lines[i + 1:] if not active.match(l)]
            lines[i] = f'{m.group(1)}{key}{m.group(2)}{value}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        if commented.match(line):
            lines[i] = f'{key} = {value}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m and m.group(1).strip().lower() == 'language':
            lines.insert(i + 1, f'{key} = {value}')
            return '\n'.join(lines) + '\n'
    if lines and lines[-1].strip():
        lines.append('')
    lines += ['[language]', f'{key} = {value}']
    return '\n'.join(lines) + '\n'


def set_language(text, lang):
    return set_value(text, 'lang', lang)


def get_value(text, key):
    active, _ = _patterns(key)
    for line in text.splitlines():
        m = active.match(line)
        if m:
            return m.group(3)
    return None


def get_language(text):
    return get_value(text, 'lang')


def write(dest, lang, example=None, game_langs=None):
    """Write dest/xc2.ini with lang and, when given, game_langs (a list of codes); an existing file keeps its other
    lines. Returns the path."""
    path = Path(dest) / 'xc2.ini'
    legacy = Path(dest) / 'xcx.ini'   # an install folder made before the rename to XC2: renamed before it is read
    if legacy.is_file() and not path.exists():
        os.replace(legacy, path)
    if path.is_file():
        text = path.read_text(encoding='utf-8-sig')
    elif example and Path(example).is_file():
        text = Path(example).read_text(encoding='utf-8')
    else:
        text = ''
    new = set_language(text, lang)
    if game_langs:
        new = set_value(new, 'game_langs', ' '.join(game_langs))
    if path.is_file() and new == text:
        return path
    tmp = path.with_name('xc2.ini.tmp')
    tmp.write_text(new, encoding='utf-8', newline='\n')
    os.replace(tmp, path)
    return path
