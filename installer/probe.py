"""Which game languages the chosen game files have: python -m installer.probe --base FILE [--update FILE] --keys FILE

The window runs this when the base game and the keys are chosen, so its language list holds only the languages that
release has text for (the Americas and Europe: English, French, German, Italian, Spanish and, from update 3.0.1,
Korean; Japan: Japanese and Chinese). It opens the files as the check step does (installer/steps.py Check.open_input)
and reads bf2.arh in place (tools/xc2_languages.py), about a second. Prints one JSON line:
    {"type": "languages", "region": "us", "release": "Americas and Europe", "languages": ["en-US", ...]}
    {"type": "error", "id": "check.keys_wrong", "params": {...}}
The update is optional: without it the list is the base game's alone. Exit code 0, or 1 after an error line.
Key values are read from the keys file and never printed.
"""
import argparse
import json
import sys
from pathlib import Path

from .runner import InstallError
from .steps import Check, game_languages, load_keys, xc2_titles


def probe(base, update, keys_path):
    """The languages event of the game files; raises InstallError with the check step's message ids."""
    for name, path in (('base', base), ('update', update), ('keys', keys_path)):
        if path is not None and not Path(path).is_file():
            raise InstallError('check.input_missing', input=name, path=str(path))
    try:
        keys = load_keys(Path(keys_path))
    except (ValueError, OSError, UnicodeDecodeError):
        raise InstallError('check.keys_unreadable') from None
    if 'header_key' not in keys:
        raise InstallError('check.keys_no_header_key')
    found = {}
    for name, path, is_update in (('base', base, False), ('update', update, True)):
        if path is None:
            continue
        with Path(path).open('rb') as f:
            found[name] = xc2_titles.of_title(Check.open_input(f, Path(path), keys, name, is_update)['title_id'])
    del keys
    region = found['base']
    if update is not None and found['update'] is not region:
        raise InstallError('check.region_mismatch', base=region.name, update=found['update'].name)
    return {'type': 'languages', 'region': region.key, 'release': region.name,
            'languages': game_languages(base, update, keys_path)}


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m installer.probe', description=__doc__.splitlines()[0])
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--update', type=Path)
    ap.add_argument('--keys', type=Path, required=True)
    args = ap.parse_args(argv)
    try:
        event = probe(args.base, args.update, args.keys)
    except InstallError as e:
        print(json.dumps({'type': 'error', 'id': e.id, 'params': {k: str(v) for k, v in e.params.items()}}))
        return 1
    except (OSError, ValueError) as e:
        print(json.dumps({'type': 'error', 'id': 'check.languages_unreadable', 'params': {'detail': str(e)}}))
        return 1
    print(json.dumps(event))
    return 0


if __name__ == '__main__':
    sys.exit(main())
