"""Extract the game named in config.local.toml into build/ and verify it against known hashes.

Steps, each skipped when its verified output already exists:

1. ExeFS (code modules) of the base game and of the update.
2. Base RomFS, verified through all six IVFC levels.
3. Update RomFS: the base RomFS patched with the update's BKTR section, read in place (tools/xc2_merge_romfs.py).

Steps 1 and 3 run for every file in `input.updates`. Each update's version is identified by the SHA-256 of its `main`,
not by its file name. Output: build/extracted/v0/{exefs,romfs} and build/extracted/v<version>/{exefs,romfs}. The base
RomFS is extracted only when the base game's expected file has a RomFS hash (XC2's has none: the update's merge reads
the base game in place).

--region jp takes the Japanese release (tools/xc2_titles.py): its expected files are tools/data/expected-xc2-jp-*.json
and its folders v0jp and v301jp. --record VERSION writes those expected files from the game files instead of checking
against them, once, for a release the repository has no hashes for yet: it extracts the base game's ExeFS and the
update's ExeFS and merged RomFS and records their hashes as the base game 1.0.0 and the update VERSION. Only the
hashes go into tools/data; check them against a second dump before committing them.

--base, --update, --keys and --work name the files and the output directory instead of the config (the installer
imports the functions instead). Runs under .venv-py314. Key values are read by the tools from the keys file and never
printed.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
# The vendored scripts import each other by bare module name, so their directory goes on the path before importing them.
sys.path.insert(0, str(VENDOR))
from extract_exefs import require  # noqa: E402
import xc2_merge_romfs  # noqa: E402
import xc2_titles  # noqa: E402

DATA = ROOT / 'tools' / 'data'
EXEFS_CODE = ('main', 'sdk', 'subsdk0', 'rtld')
# Errors of the vendored extractors (tools/vendor/macnativegames) that mean a game file's data does not match its own
# hashes or ends early: the file is damaged or was not copied completely. DAMAGE finds them in a script's log.
DAMAGE_TEXT = re.compile(r'hash mismatch|hash-table mismatch|FS header hash|IVFC level \d+ block \d+ failed|Truncated|'
                         r'out of bounds|Section bounds')
DAMAGE = re.compile(rf'^\w*Error: (.*(?:{DAMAGE_TEXT.pattern}).*)$', re.M)


class DamagedFile(ValueError):
    """A game file whose data fails its hash checks or ends early. path: that file; detail: the check that failed."""

    def __init__(self, path, detail):
        self.path, self.detail = Path(path), detail
        super().__init__(f'{self.path} is damaged or incomplete ({detail}). Copy that file again from its source '
                         f'(or dump it again) and retry.')


def say(message):
    print(message, flush=True)


def shown(path):
    """path relative to the repository when it is below it (log lines), else as given."""
    return path.relative_to(ROOT) if path.is_relative_to(ROOT) else path


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def load_expected(region=xc2_titles.DEFAULT):
    """{display version: expected hashes} of one release (tools/xc2_titles.py)."""
    versions = {}
    for path in sorted(DATA.glob('expected-xc2-*.json')):
        data = json.loads(path.read_text())
        if path.name == region.expected_name(data['display_version']):
            versions[data['display_version']] = data
    return versions


def version_dir(expected, region=xc2_titles.DEFAULT):
    return 'v0' + region.suffix if expected['version'] == 0 else region.version_tag(expected['display_version'])


# The embeddable Python (a ._pth file) does not put a script's directory on sys.path, so a vendored script that imports
# its siblings fails when started by path. This stub adds the directory and tools/, then package_select.run() starts the
# script (a vendored one behind its combined-XCI base check).
RUN_SCRIPT = ('import os, sys; p = sys.argv[1]; sys.path.insert(0, os.path.dirname(p)); '
              f'sys.path.insert(0, {str(Path(__file__).resolve().parent)!r}); sys.argv = sys.argv[1:]; '
              'import package_select; package_select.run(p)')


def run_logged(command, log, source=None):
    """Run a vendored script with its output in log. source: the game file it reads; a failed hash check or a short
    read of that file raises DamagedFile."""
    args = [str(c) for c in command]
    env = dict(os.environ, PYTHONIOENCODING='utf-8')   # its messages may name non-ASCII paths
    with log.open('wb') as out:
        code = subprocess.run([args[0], '-c', RUN_SCRIPT, *args[1:]], stdout=out, stderr=subprocess.STDOUT,
                              env=env).returncode
    if code and source:
        damage = DAMAGE.search(log.read_text(encoding='utf-8', errors='replace'))
        if damage:
            raise DamagedFile(source, damage.group(1).strip())
    require(code == 0, f'{Path(command[1]).name} failed; see {shown(log)}')


def exefs_problems(exefs, expected):
    """Code modules must match byte for byte. main.npdm differs between dumps that a repacking tool re-signed."""
    problems = []
    for name in EXEFS_CODE:
        path = exefs / name
        if not path.is_file() or sha256(path) != expected['exefs_sha256'][name]:
            problems.append(name)
    return problems


def extract_exefs(game, keys, out, logs, *, update):
    exefs = out / 'exefs'
    if exefs.exists():
        shutil.rmtree(exefs)
    out.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, VENDOR / 'extract_exefs.py', game, '--keys', keys, '--output', exefs]
    run_logged(command + (['--update'] if update else []), logs / f'extract_exefs-{out.name}.log', source=game)
    return exefs


def identify_update(exefs, versions):
    digest = sha256(exefs / 'main')
    for expected in versions.values():
        if expected['version'] and expected['exefs_sha256']['main'] == digest:
            return expected
    raise ValueError(f'Update main {digest[:16]}... matches no supported version ({", ".join(versions)})')


def report_npdm(exefs, expected):
    if sha256(exefs / 'main.npdm') != expected['exefs_sha256']['main.npdm']:
        say(f'  main.npdm differs from the reference {expected["display_version"]} dump; only the code modules are used')


def romfs_verified(romfs, expected):
    index = romfs / 'index.json'
    if not index.is_file() or not (romfs / 'romfs.bin').is_file():
        return False
    report = json.loads(index.read_text())
    return report.get('romfs_sha256') == expected['romfs']['sha256'] and report.get('romfs_size') == expected['romfs']['size']


def check_romfs(romfs, expected):
    report = json.loads((romfs / 'index.json').read_text())
    require(report['romfs_size'] == expected['romfs']['size'] and report['romfs_sha256'] == expected['romfs']['sha256'],
            f'RomFS does not match {expected["display_version"]}: size {report["romfs_size"]}, '
            f'sha256 {report["romfs_sha256"][:16]}...')
    say(f'  RomFS {expected["display_version"]}: {report["romfs_size"]:,} bytes, {len(report["files"]):,} files, hash matches')


def extract_base_romfs(out, keys, logs, base=None):
    romfs = out / 'romfs'
    if romfs.exists():
        shutil.rmtree(romfs)
    run_logged([sys.executable, VENDOR / 'extract_romfs.py', '--keys', keys,
                '--manifest', out / 'extraction-manifest.json', '--output', romfs], logs / 'extract_romfs-v0.log',
               source=base)


MERGE_SLACK = 1 << 30   # beside the merged image: index.json and the file system's own overhead


def merge_need(romfs_size):
    """Free bytes merge_update_romfs needs where its output goes: the merged image (it reads the game files in place)."""
    return romfs_size + MERGE_SLACK


def merge_update_romfs(base, update, keys_path, output, progress=None):
    """Write output/romfs.bin and output/index.json: the base RomFS patched with the update's BKTR section, read in
    place from both game files (tools/xc2_merge_romfs.py). progress(done, total) follows the bytes written. The caller
    checks the SHA-256 in index.json against the expected one."""
    for stale in (output, output.with_name(f'.{output.name}.part')):
        if stale.exists():
            shutil.rmtree(stale)
    output.parent.mkdir(parents=True, exist_ok=True)
    expected = max((e['romfs']['size'] for r in xc2_titles.REGIONS for e in load_expected(r).values()
                    if e['version']), default=0)
    need = merge_need(expected)
    require(shutil.disk_usage(output.parent).free > need, f'Need {need / (1 << 30):.0f} GiB free in {output.parent}')
    try:
        xc2_merge_romfs.merge(base, update, keys_path, output, progress)
    except xc2_merge_romfs.ShortRead as e:
        raise DamagedFile(e.path, 'it ends before the end of its game data') from None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.toml')
    parser.add_argument('--base', type=Path, help='base game (default: input.base of the config)')
    parser.add_argument('--update', type=Path, action='append',
                        help='an update NSP, repeatable (default: input.updates of the config)')
    parser.add_argument('--keys', type=Path, help='prod.keys (default: input.keys of the config)')
    parser.add_argument('--work', type=Path, help='output directory with extracted/ and logs/ (default: work.dir of '
                                                  'the config, else build)')
    parser.add_argument('--region', choices=sorted(xc2_titles.BY_KEY), default=xc2_titles.DEFAULT.key,
                        help='the release of the game files: us (the Americas and Europe, default) or jp')
    parser.add_argument('--record', metavar='VERSION',
                        help="write the release's expected files from these game files, the update being VERSION")
    args = parser.parse_args()
    region = xc2_titles.BY_KEY[args.region]

    config = {}
    if not (args.base and args.keys and args.update and args.work):
        config = tomllib.loads(args.config.read_text(encoding='utf-8'))
    given = config.get('input', {})
    base, keys = args.base or Path(given['base']), args.keys or Path(given['keys'])
    updates = args.update or given.get('updates', [])
    work = args.work or ROOT / config.get('work', {}).get('dir', 'build')
    extracted, logs = work / 'extracted', work / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    if args.record:
        require(len(updates) == 1, '--record takes one update')
        record(region, args.record, base, Path(updates[0]), keys, extracted, logs)
        return
    versions = load_expected(region)
    require('1.0.0' in versions, f'No expected hashes for the {region.name} release in {shown(DATA)}; '
                                 f'see --record')
    v0 = versions['1.0.0']

    say(f'Base game ({region.name} release)')
    base_out = extracted / version_dir(v0, region)
    if exefs_problems(base_out / 'exefs', v0):
        say('  extracting ExeFS')
        extract_exefs(base, keys, base_out, logs, update=False)
    problems = exefs_problems(base_out / 'exefs', v0)
    require(not problems, f'Base game is not XC2 1.0.0: {", ".join(problems)} differ')
    say('  ExeFS 1.0.0: main, sdk, subsdk0, rtld match')
    report_npdm(base_out / 'exefs', v0)
    if 'romfs' in v0:   # XC2's base has no RomFS hash: only the update's merged RomFS is used
        if not romfs_verified(base_out / 'romfs', v0):
            say('  extracting RomFS (17 GB)')
            extract_base_romfs(base_out, keys, logs, base)
        check_romfs(base_out / 'romfs', v0)

    for update in updates:
        extract_update(base, Path(update), keys, extracted, logs, versions, region)


def record(region, version, base, update, keys, extracted, logs):
    """Write tools/data's expected files of a release from its game files (main --record)."""
    files = {v: DATA / region.expected_name(v) for v in ('1.0.0', version)}
    require(not any(f.exists() for f in files.values()), f'{", ".join(f.name for f in files.values() if f.exists())} '
                                                         'already exist; remove them to record again')
    exefs_names = (*EXEFS_CODE, 'main.npdm')
    base_out, update_out = extracted / ('v0' + region.suffix), extracted / region.version_tag(version)
    say(f'Recording the {region.name} release: base game {base.name}, update {update.name} as {version}')
    extract_exefs(base, keys, base_out, logs, update=False)
    extract_exefs(update, keys, update_out, logs, update=True)
    for name, out in (('base game', base_out), ('update', update_out)):
        title = json.loads((out / 'extraction-manifest.json').read_text())['program']['title_id'].upper()
        require(xc2_titles.of_title(title) is region, f'The {name} is title {title}, not the {region.name} release '
                                                      f'({region.title}); pass the --region of the files')
    say('  merging RomFS with the update')
    merge_update_romfs(base, update, keys, update_out / 'romfs')
    report = json.loads((update_out / 'romfs' / 'index.json').read_text())
    common = {'title_id': region.title, 'base_title_id': region.title}
    entries = {
        '1.0.0': {'_comment': f'Xenoblade Chronicles 2 base game 1.0.0 ({region.name} release, {region.title}), '
                              'ExeFS of the base Program NCA (tools/extract.py --record).',
                  **common, 'version': 0, 'display_version': '1.0.0',
                  'exefs_sha256': {n: sha256(base_out / 'exefs' / n) for n in sorted(exefs_names)}},
        version: {'_comment': f'Xenoblade Chronicles 2 {version} ({region.name} release, base {region.title} + update '
                              f'{region.update_title}), ExeFS of the update Program NCA, romfs the BKTR merge of the '
                              'base and update RomFS (tools/extract.py --record).',
                  **common, 'title_id': region.update_title, 'version': version, 'display_version': version,
                  'exefs_sha256': {n: sha256(update_out / 'exefs' / n) for n in sorted(exefs_names)},
                  'romfs': {'size': report['romfs_size'], 'sha256': report['romfs_sha256']}},
    }
    for v, path in files.items():
        path.write_text(json.dumps(entries[v], indent=2) + '\n', encoding='utf-8')
        say(f'  wrote {shown(path)}')


def extract_update(base, update, keys, extracted, logs, versions, region=xc2_titles.DEFAULT):
    say(f'Update {update.name}')
    staging = extracted / 'update-staging'
    known = [d for d in extracted.glob('v*') if d.name != 'v0' + region.suffix and
             (d / 'extraction-manifest.json').is_file()]
    current = next((d for d in known if json.loads((d / 'extraction-manifest.json').read_text())['source']
                    == str(update.resolve())), None)
    if current is None or not any(not exefs_problems(current / 'exefs', e) for e in versions.values() if e['version']):
        say('  extracting ExeFS')
        exefs = extract_exefs(update, keys, staging, logs, update=True)
        expected = identify_update(exefs, versions)
        current = extracted / version_dir(expected, region)
        if current.exists():
            shutil.rmtree(current)
        staging.rename(current)
    expected = identify_update(current / 'exefs', versions)
    problems = exefs_problems(current / 'exefs', expected)
    require(not problems, f'Update {expected["display_version"]}: {", ".join(problems)} differ')
    say(f'  ExeFS {expected["display_version"]}: main, sdk, subsdk0, rtld match')
    report_npdm(current / 'exefs', expected)
    if not romfs_verified(current / 'romfs', expected):
        say('  merging RomFS with the update')
        merge_update_romfs(base, update, keys, current / 'romfs')
    check_romfs(current / 'romfs', expected)


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        sys.exit(f'error: {error}')
