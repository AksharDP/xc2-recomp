"""The eight setup steps of docs/installer.md: check, kit, extract, dlc, lift, compile, prepare, cleanup.

Extraction runs in this process through tools/extract.py and tools/xc2_merge_romfs.py (the package's sibling tools/,
which ships with it), reading the game files in place; lift and compile run the bundled Python on recomp/gen.py and tools/build_game.py, whose output goes to
<temp>/logs/<step>-<tool>.log and is read for progress. No Win32 call here: the host check is a small C program built
with the bundled clang (hostcheck.c).

Install folder: exefs/, romfs/ (romfs.bin, index.json), aoc/<index>/ (the add-on content's RomFS), profile/, xc2.exe
and its DLLs, romfs-index.bin, xc2_xlate.exe, xc2.ini, xc2-install.json, mods/ (the player's, never written) and
build-cache/ (modcode.txt: the command the game runs to build the mods' code; mod-base/: what that needs of the lift
and the exe's x86-64 level;
mods/, cheats.status, exefs.status, exefs.data: what it made, runtime/modcode.c; objects/: the generated objects the
next build reuses, tools/build_game.py ObjectCache; work/: the temp folder) and build-kit/ (Kit).
Temp folder (installer/layout.py temp_for): check/ (ExeFS of both inputs, the host check), gen/ (C and the lift cache),
build/ (objects and the ThinLTO cache), logs/. The cleanup keeps logs/.
"""
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import ini
from . import layout as layouts
from .layout import BASE, EXE, GAME_EXE, LEGACY_EXE, SUPPORTED, version_tag
from .runner import InstallError, Step

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import extract  # noqa: E402
import xc2_languages  # noqa: E402
import xc2_titles  # noqa: E402
from extract_exefs import (common_tickets, container_entries, load_keys, nca_ctr_key, nca_inventory)  # noqa: E402
from package_select import program_nca  # noqa: E402

GiB = 1 << 30
DEST_EXTRA = 4 * GiB      # beside the RomFS: shader and pipeline caches (about 2 GB), exefs, xc2.exe
TEMP_NEED = 3 * GiB       # generated C (about 0.9 GB) and objects
REBUILD_NEED = 1 * GiB    # what a rebuild writes beside the generated C and objects the last build left
OBJECT_CACHE = 2 * GiB    # build-cache/objects in the install folder: the generated objects kept for the next build
KIT_NEED = 1 * GiB        # the build kit (tools/make_release.py's folder without XC2-Setup.exe)
HOSTCHECK = Path(__file__).resolve().parent / 'hostcheck.c'
STAMP = '.install-key'
EXE_WAIT = 20   # seconds the compile step waits for a game that is closing to let go of its exe
# The code patch sets of an update (recomp/patches.py) are in recomp/<Region.patch_table>: XC2's frame rate and game
# resolution (patches-v301.txt). patches-upd.txt beside it is XCX 2.0.0's and must never reach XC2's code.
XLATE = 'xc2_xlate' + EXE   # the translator's name beside xc2.exe; runtime/gfx_shader.c starts it by this name


# ---- helpers

def file_sha256(path, progress=None):
    """SHA-256 hex of a file; progress(done, total) after each block."""
    h = hashlib.sha256()
    total = path.stat().st_size
    done = 0
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(8 << 20)
        view = memoryview(buf)
        while n := f.readinto(buf):
            h.update(view[:n])
            done += n
            if progress:
                progress(done, total)
    return h.hexdigest()


def fingerprint(*parts):
    """SHA-256 over strings and the contents of files and directory trees (a missing path counts as missing)."""
    h = hashlib.sha256()
    for p in parts:
        if isinstance(p, Path):
            if p.is_dir():
                for f in sorted(p.rglob('*')):
                    if f.is_file() and '__pycache__' not in f.parts:
                        h.update(f.relative_to(p).as_posix().encode() + b'\0' + bytes.fromhex(file_sha256(f)))
            elif p.is_file():
                h.update(p.name.encode() + b'\0' + bytes.fromhex(file_sha256(p)))
            else:
                h.update(b'missing\0' + p.name.encode())
        else:
            h.update(str(p).encode())
        h.update(b'\1')
    return h.hexdigest()


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def replace_file(src, dst):
    """Copy src over dst through a temporary name, so a cancelled copy never leaves half a file under dst's name."""
    tmp = dst.with_name(dst.name + '.tmp')
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
        return
    try:   # run_tool starts each tool in a session of its own: the group holds ninja, clang and the game
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        proc.kill()


def linux_glibc():
    """(major, minor) of the C library this Linux process runs on, None elsewhere or when it is not glibc."""
    try:
        name = os.confstr('CS_GNU_LIBC_VERSION') if sys.platform.startswith('linux') else None
    except (ValueError, OSError):
        return None
    m = re.match(r'glibc (\d+)\.(\d+)', name or '')
    return (int(m.group(1)), int(m.group(2))) if m else None


# The bundled clang, lld and the cryptography wheel are built against glibc 2.34 (tools/make_release_linux.py
# GLIBC_FLOOR); on an older one they cannot start
GLIBC_NEEDED = (2, 34)


def linux_kernel():
    """(major, minor) of the running Linux kernel, None elsewhere."""
    if not sys.platform.startswith('linux'):
        return None
    m = re.match(r'(\d+)\.(\d+)', os.uname().release)
    return (int(m.group(1)), int(m.group(2))) if m else None


def run_tool(ctx, step, script, args, parse=None, cwd=None):
    """Run a tool script with the bundled Python. Its output goes to <temp>/logs/<step>-<script>.log and, line by line,
    to parse(line). Returns (exit code, log path)."""
    logs = ctx.temp / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    log = logs / f'{step}-{Path(script).stem}.log'
    command = [str(ctx.layout.python), '-c', extract.RUN_SCRIPT, str(script), *map(str, args)]
    ctx.reporter.log('run ' + ' '.join(command[3:]))
    env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
    for var in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS'):
        env.setdefault(var, '1')
    with open(log, 'w', encoding='utf-8', newline='\n', buffering=1) as out:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd or ctx.layout.root,
                                env=env, text=True, encoding='utf-8', errors='replace', bufsize=1,
                                start_new_session=os.name != 'nt')
        try:
            for line in proc.stdout:
                out.write(line)
                if parse:
                    parse(line.rstrip())
            code = proc.wait()
        except BaseException:
            kill_tree(proc)
            raise
    ctx.reporter.log(f'{Path(script).name}: exit code {code}; output in {log}')
    return code, log


def free_bytes(path):
    """Free bytes on the disk of path (its nearest existing parent) and that parent's device."""
    p = Path(path)
    while not p.exists():
        if p.parent == p:   # a drive or share that is not there: its root is its own parent
            raise InstallError('check.dest_unreachable', path=str(path))
        p = p.parent
    return shutil.disk_usage(p).free, os.stat(p).st_dev


def raise_if_damaged(error, name, path):
    """InstallError check.input_damaged when a reader's ValueError says the file's data fails its hashes or ends early
    (extract.DAMAGE_TEXT)."""
    if extract.DAMAGE_TEXT.search(str(error)):
        raise InstallError('check.input_damaged', input=name, path=str(path), detail=str(error)) from None


def game_languages(base, update, keys_path):
    """The game languages the game files have text for, read in place (tools/xc2_languages.py): the base game patched
    by update (None: the base alone). The two releases differ, so this is what the window offers and the check takes."""
    try:
        return xc2_languages.languages(xc2_languages.from_game(Path(base), Path(keys_path),
                                                               Path(update) if update else None))
    except ValueError as e:
        raise_if_damaged(e, 'base', base)
        raise InstallError('check.languages_unreadable', detail=str(e)) from None


def installed_languages(dest):
    """The game languages of an installed RomFS (dest/romfs, with the update applied), or None without one."""
    romfs = Path(dest) / 'romfs'
    if not (romfs / 'index.json').is_file():
        return None
    try:
        return xc2_languages.languages(xc2_languages.from_romfs(romfs))
    except (OSError, ValueError, KeyError, StopIteration):
        return None


def game_version(ctx):
    """The update this install is built from: what the check identified, else the recorded one."""
    return ctx.state.get('version', SUPPORTED[0])


def game_region(ctx):
    """The release this install is built from (tools/xc2_titles.py): what the check identified from the base game's
    title id, else the recorded one. A folder from before releases were told apart holds the Americas and Europe's."""
    found = getattr(ctx, 'region', None)
    return found or xc2_titles.BY_KEY.get(ctx.state.get('region'), xc2_titles.DEFAULT)


def expected_of(ctx, version, region=None):
    """The expected hashes of the base game (BASE) or an update of the install's release
    (tools/data/expected-xc2[-jp]-<version>.json)."""
    return json.loads(ctx.layout.expected(version, region or game_region(ctx)).read_text(encoding='utf-8'))


def known_versions(ctx, region=None):
    """The updates whose expected hashes tools/data holds for the release (installer/layout.py Layout.versions)."""
    return ctx.layout.versions(region or game_region(ctx))


def expected_update(ctx):
    return expected_of(ctx, game_version(ctx))


class Watch(threading.Thread):
    """Polls a measure() of bytes written while a long call runs and reports it as progress."""

    def __init__(self, reporter, step):
        super().__init__(daemon=True)
        self.reporter, self.step = reporter, step
        self.phase, self.measure, self.total = None, None, None
        self.stopped = threading.Event()

    def set(self, phase, measure=None, total=None):
        self.phase, self.measure, self.total = phase, measure, total
        self.reporter.progress(self.step, phase, 0 if total else None, total, 'bytes' if total else None, force=True)

    def run(self):
        while not self.stopped.wait(1.0):
            phase, measure, total = self.phase, self.measure, self.total
            if phase and measure:
                try:
                    done = min(measure(), total)
                except OSError:
                    continue
                self.reporter.progress(self.step, phase, done, total, 'bytes')

    def stop(self):
        self.stopped.set()


# ---- 1. check

class Check(Step):
    id = 'check'
    always = True

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        missing = layout.missing()
        if missing:
            raise InstallError('check.release_incomplete', paths=missing[:5])
        if dest.exists() and not dest.is_dir():
            raise InstallError('check.dest_not_folder', path=str(dest))
        own = (ctx.temp, *ctx.temp.parents)   # a temp folder inside the install folder (installer/cli.py) is ours
        if ctx.mode == 'install' and dest.is_dir() and any(p not in own for p in dest.iterdir()):
            raise InstallError('check.dest_not_empty', path=str(dest))
        # The language: the one asked for, else the recorded one as this release names it (an earlier install's nl,
        # pt or ru, which no release has, gives way to the default of the game's languages in self.language)
        requested, recorded = ctx.options.lang, ini.recorded_language(ctx.state.get('lang'))
        for code in (requested, recorded):
            if code and code not in ini.LANGUAGES:
                raise InstallError('check.lang_unknown', lang=code, known=', '.join(ini.LANGUAGES))
        ctx.languages = None
        ctx.temp.mkdir(parents=True, exist_ok=True)

        rep.progress('check', 'host', force=True)
        self.host(ctx)
        rep.progress('check', 'verify', force=True)
        ctx.verify_kept('check')
        if ctx.will_run('extract'):
            rep.progress('check', 'inputs', force=True)
            self.inputs(ctx)
        self.space(ctx)
        if ctx.will_run('extract'):
            rep.progress('check', 'versions', force=True)
            self.versions(ctx)
        elif dlc_inputs(ctx):   # add-ons added to an installed folder: inputs() did not see them
            self.dlc_files(ctx)
        self.language(ctx, requested, recorded)
        return {}

    def language(self, ctx, requested, recorded=None):
        """ctx.lang: requested, else the recorded one when the game files have it, else the default of their
        languages; it must be one the game files have text for. The languages come from the game files when the
        extraction runs (inputs), else from the installed RomFS, else from the list the install recorded (an
        interrupted replacement can leave the RomFS of other game files than that list was read from)."""
        langs = ctx.languages or installed_languages(ctx.dest) or ctx.state.get('languages')
        if recorded and langs and recorded not in langs:
            recorded = None   # e.g. fr kept from the Americas release after a switch to the Japanese one
        lang = requested or recorded or (ini.default_language(langs) if langs else None)
        if not lang:
            raise InstallError('check.lang_missing')
        if langs and lang not in langs:
            raise InstallError('check.lang_not_in_game', lang=lang, known=', '.join(langs))
        if lang in ini.NEED_SHARED_FONTS:
            ctx.reporter.warning('warn.lang_needs_fonts', lang=lang)
        ctx.lang, ctx.languages = lang, langs

    def dlc_files(self, ctx):
        """Add-on files chosen without the game files: they need the keys too (their NCAs are encrypted), and each must
        hold this game's add-on content. Stops before any step writes."""
        o = ctx.options
        if not o.keys:
            raise InstallError('check.dlc_keys_needed')
        if not Path(o.keys).is_file():
            raise InstallError('check.input_missing', input='keys', path=str(o.keys))
        try:
            keys = load_keys(Path(o.keys))
        except (ValueError, OSError, UnicodeDecodeError):
            raise InstallError('check.keys_unreadable') from None
        if 'header_key' not in keys:
            raise InstallError('check.keys_no_header_key')
        for path in dlc_inputs(ctx):
            if not path.is_file():
                raise InstallError('check.input_missing', input='dlc', path=str(path))
            with path.open('rb') as f:
                dlc_record(f, path, keys, game_region(ctx))
        del keys

    def host(self, ctx):
        layout, work = ctx.layout, ctx.temp / 'check'
        glibc = linux_glibc()
        if glibc and glibc < GLIBC_NEEDED and not layout.dev:
            raise InstallError('check.glibc_old', version='.'.join(map(str, glibc)),
                               need='.'.join(map(str, GLIBC_NEEDED)))
        work.mkdir(parents=True, exist_ok=True)
        exe = work / ('hostcheck' + EXE)
        command = [str(layout.clang), '-O1', str(HOSTCHECK), '-o', str(exe)]
        if layout.deps:
            command += ['-I', str(layout.deps / 'include')]
        if layout.sysroot:
            command += [f'--sysroot={layout.sysroot}', '-fuse-ld=lld']
        if os.name != 'nt':
            command.append('-ldl')
        r = subprocess.run(command, capture_output=True, text=True, errors='replace')
        if r.returncode:
            ctx.reporter.log(r.stdout + r.stderr)
            raise InstallError('check.toolchain_broken', detail=(r.stderr or r.stdout).strip()[-400:])
        r = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace')
        try:
            host = json.loads(r.stdout)
        except ValueError:
            raise InstallError('check.host_probe_failed', code=r.returncode) from None
        ctx.reporter.log(f'host: {r.stdout.strip()}')
        if not host['x86_64_v3']:
            raise InstallError('check.cpu_no_avx2')
        if not host['loader']:
            raise InstallError('check.vulkan_missing')
        devices = host['devices']
        if not devices:
            raise InstallError('check.vulkan_no_device')
        if not any(tuple(d['api'][:2]) >= (1, 3) for d in devices):
            raise InstallError('check.vulkan_too_old',
                               devices=', '.join(f'{d["name"]} ({d["api"][0]}.{d["api"][1]})' for d in devices))
        kernel = linux_kernel()
        if kernel and kernel < (6, 7):
            # Without userfaultfd's asynchronous write protection nothing tracks the CPU's writes: the game copies whole
            # pools to the GPU at each submit and its texture checks see every page as changed (docs/building.md, Linux)
            ctx.reporter.warning('warn.kernel_old', version='.'.join(map(str, kernel)))
        ctx.host = host

    def inputs(self, ctx):
        """Open both files with the keys as the extraction will."""
        o = ctx.options
        for name in ('base', 'update', 'keys'):
            path = getattr(o, name)
            if not path:
                raise InstallError('check.inputs_needed')
            if not Path(path).is_file():
                raise InstallError('check.input_missing', input=name, path=str(path))
        try:
            keys = load_keys(Path(o.keys))
        except (ValueError, OSError, UnicodeDecodeError):
            raise InstallError('check.keys_unreadable') from None
        if 'header_key' not in keys:
            raise InstallError('check.keys_no_header_key')
        found = {}
        for name, is_update in (('base', False), ('update', True)):
            path = Path(getattr(o, name))
            with path.open('rb') as f:
                found[name] = xc2_titles.of_title(self.open_input(f, path, keys, name, is_update)['title_id'])
        # The release comes from the base game's title id: the Americas and Europe share one, Japan has its own
        # (tools/xc2_titles.py). The update must be that release's, and tools/data must hold its expected hashes.
        region = found['base']
        if found['update'] is not region:
            raise InstallError('check.region_mismatch', base=region.name, update=found['update'].name)
        if not ctx.layout.supports(region):
            raise InstallError('check.region_unsupported', region=region.name, title=region.title)
        ctx.region = region
        ctx.reporter.log(f'check: {region.name} release ({region.title})')
        for path in dlc_inputs(ctx):   # optional: each must hold this release's add-on content
            if not path.is_file():
                raise InstallError('check.input_missing', input='dlc', path=str(path))
            with path.open('rb') as f:
                dlc_record(f, path, keys, region)
        del keys
        ctx.languages = game_languages(o.base, o.update, o.keys)
        ctx.reporter.log(f'check: the game files have text for {" ".join(ctx.languages)}')

    @staticmethod
    def open_input(f, path, keys, name, is_update):
        try:
            kind, entries = container_entries(f, path.stat().st_size)
        except ValueError as e:
            raise_if_damaged(e, name, path)
            raise InstallError('check.not_game_file', input=name) from None
        if is_update and kind != 'nsp':
            raise InstallError('check.update_not_nsp')
        try:
            tickets = common_tickets(f, entries)
        except ValueError as e:
            if 'Personalized' in str(e):
                raise InstallError('check.ticket_personalized', input=name) from None
            raise_if_damaged(e, name, path)
            raise InstallError('check.file_unreadable', input=name, detail=str(e)) from None
        try:
            records = nca_inventory(f, entries, keys)
        except ValueError as e:
            if 'NCA3' in str(e):
                raise InstallError('check.keys_wrong', input=name) from None
            raise_if_damaged(e, name, path)
            raise InstallError('check.file_unreadable', input=name, detail=str(e)) from None
        try:
            record, header = program_nca(records, update=is_update)
        except ValueError as e:
            if is_update and 'BKTR' in str(e):
                raise InstallError('check.update_is_base') from None
            raise InstallError('check.update_not_game' if is_update else 'check.base_not_game', detail=str(e)) from None
        try:
            nca_ctr_key(header, keys, tickets)
        except ValueError as e:
            if 'ticket' in str(e):
                raise InstallError('check.ticket_missing', input=name) from None
            raise InstallError('check.keys_too_old', input=name,
                               generation=max(header[0x206], header[0x220])) from None
        return record

    def space(self, ctx):
        # The steps run one after another and each frees its scratch files before the next starts, so the install
        # folder needs the largest of the peaks below, not their sum:
        #   extract  the merged RomFS, written once from the game files read in place (tools/extract.py
        #            merge_update_romfs, which checks merge_need itself)
        #   dlc      the RomFS, the packs already unpacked and, for a moment, the images of the DLC_JOBS packs being
        #            extracted beside their files (tools/xc2_extract.py dlc_romfs deletes the image once unpacked)
        #   after    the RomFS, every pack, the object cache of the first build (OBJECT_CACHE) and the caches the game
        #            writes beside them (DEST_EXTRA)
        romfs_size = kept = 0
        if ctx.will_run('extract'):
            # the update is identified after this estimate: the larger RomFS of the supported ones
            romfs_size = max(expected_of(ctx, v)['romfs']['size'] for v in known_versions(ctx))
            romfs = ctx.dest / 'romfs' / 'romfs.bin'
            if romfs.is_file():   # a resumed extraction deletes it first, which frees its space
                kept = romfs.stat().st_size
        extract_peak = extract.merge_need(romfs_size) - kept if ctx.will_run('extract') else 0
        packs = [p.stat().st_size for p in dlc_inputs(ctx) if p.is_file()] if ctx.will_run('dlc') else []
        settled = romfs_size - kept + sum(packs)
        dlc_peak = settled + sum(sorted(packs)[-DLC_JOBS:])   # DLC_JOBS images at once beside the unpacked files
        cache = OBJECT_CACHE if ctx.will_run('compile') and not object_cache(ctx).is_dir() else 0
        kit = KIT_NEED if ctx.will_run('kit') and layouts.uses_kit(ctx.layout) else 0   # copied before the extraction
        after = settled + cache + (DEST_EXTRA if ctx.will_run('prepare') or ctx.will_run('extract') else 0)
        dest_need = kit + max(extract_peak, dlc_peak, after, 0)
        kept_work = (ctx.temp / 'gen' / 'layout.json').is_file() and (ctx.temp / 'build').is_dir()
        temp_need = (REBUILD_NEED if kept_work else TEMP_NEED) if ctx.will_run('lift') or ctx.will_run('compile') else 0
        dest_free, dest_dev = free_bytes(ctx.dest)
        temp_free, temp_dev = free_bytes(ctx.temp)
        ctx.reporter.log(f'space: install folder needs {dest_need / GiB:.1f} GiB of {dest_free / GiB:.1f} free, '
                         f'temp folder {temp_need / GiB:.1f} GiB of {temp_free / GiB:.1f}')
        if dest_dev == temp_dev:   # lift and compile run after extract and dlc, beside what those left
            dest_need = max(dest_need, kit + max(dlc_peak, after) + temp_need)
            temp_need = 0
        ctx.reporter.emit('space', dest=str(ctx.dest), need=dest_need, free=dest_free, temp=str(ctx.temp),
                          temp_need=temp_need, temp_free=temp_free, same_disk=dest_dev == temp_dev)
        for path, need, free in ((ctx.dest, dest_need, dest_free), (ctx.temp, temp_need, temp_free)):
            if need > free:
                raise InstallError('check.disk_space', path=str(path), need_gb=round(need / 1e9, 1),
                                   free_gb=round(free / 1e9, 1))

    def versions(self, ctx):
        """Extract both ExeFS into the temp folder (verifying every block) and identify the versions by hash."""
        o, work = ctx.options, ctx.temp / 'check'
        logs = ctx.temp / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        region = game_region(ctx)
        versions = {v: expected_of(ctx, v) for v in known_versions(ctx)}
        for name, is_update in (('base', False), ('update', True)):
            out = work / name
            remove(out)
            try:
                extract.extract_exefs(Path(getattr(o, name)), Path(o.keys), out, logs, update=is_update)
            except extract.DamagedFile as e:
                raise InstallError('check.input_damaged', input=name, path=str(e.path), detail=e.detail) from None
            except ValueError:
                raise InstallError('check.exefs_failed', input=name, log=str(logs / f'extract_exefs-{name}.log')) \
                    from None
        problems = extract.exefs_problems(work / 'base' / 'exefs', expected_of(ctx, BASE))
        if problems:
            raise InstallError('check.base_wrong_version', modules=', '.join(problems))
        try:
            found = extract.identify_update(work / 'update' / 'exefs', versions)['version']
        except ValueError:
            found = 'unknown'
        if found not in versions:
            raise InstallError('check.update_wrong_version', found=found, wanted=' / '.join(versions))
        recorded = xc2_titles.BY_KEY.get(ctx.state.get('region'), xc2_titles.DEFAULT)
        # another update or another release than the recorded one: every step builds again from it
        if found != ctx.state.get('version', SUPPORTED[0]) or region is not recorded:
            ctx.reporter.log(f'check: update {found} of the {region.name} release (recorded '
                             f'{ctx.state.get("version", SUPPORTED[0])} of the {recorded.name} release)')
            ctx.state['version'], ctx.state['region'] = found, region.key
            ctx.keys = {}
            ctx.plan(ctx.forced | {'extract', 'dlc', 'lift', 'compile', 'prepare'})
        ctx.state['version'], ctx.state['region'] = found, region.key


# ---- 1b. build kit

class Kit(Step):
    """The build kit (installer/layout.py, Windows release): the release folder without XC2-Setup.exe, copied into
    <install>/build-kit. Every step after this one runs its tools from there (settle), and the game builds its mods'
    code with it (mod_command), so the setup folder can go once the install is done and the install folder can move.
    A newer setup replaces the kit. The steps' keys fingerprint file contents, not paths, so the kit, a copy of the
    setup's files, keeps every step the setup recorded. (This step and adopt_old_temp are adapted from totk-pc-recomp
    installer/steps.py @ 8c9c18a and 35c590b, MIT, Copyright (c) 2026 Valiantsin Dzerakh.)"""

    id = 'kit'

    def key(self, ctx):
        return fingerprint('kit-1', ctx.layout.release if layouts.uses_kit(ctx.layout) else 'none')

    def current(self, ctx, record):
        if not layouts.uses_kit(ctx.layout):
            return True
        kit = layouts.kit_folder(ctx.dest)
        if not layouts.is_release(kit):
            return False
        found = layouts.release_layout(kit)
        return found.release == ctx.layout.release and not found.missing()

    def run(self, ctx):
        layout, rep = ctx.layout, ctx.reporter
        if not layouts.uses_kit(layout):
            return {}
        kit = layouts.kit_folder(ctx.dest)
        if layout.root.resolve() == kit.resolve():   # run from the kit: it is in place
            return {}
        new, old = kit.with_name(layouts.KIT + '.new'), kit.with_name(layouts.KIT + '.old')
        try:
            remove(new)   # what a cancelled or failed run left
            remove(old)
        except OSError as e:
            raise InstallError('kit.in_use', path=str(e.filename or old)) from None
        files = []
        for name in layouts.KIT_ITEMS:
            src = layout.root / name
            if src.is_dir():
                files += [f for f in sorted(src.rglob('*')) if f.is_file() and '__pycache__' not in f.parts]
            elif src.is_file():
                files.append(src)
        total, done = sum(f.stat().st_size for f in files), 0
        rep.progress('kit', 'copy', 0, total, 'bytes', force=True)
        try:
            for f in files:
                dst = new / f.relative_to(layout.root)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
                done += f.stat().st_size
                rep.progress('kit', 'copy', done, total, 'bytes')
        except OSError as e:
            raise InstallError('kit.copy_failed', path=str(e.filename or new), detail=str(e)) from None
        # A game that is building its mods holds files of the old kit: then nothing is replaced, and the message asks to
        # close the game and run the setup again
        moved = False
        try:
            if kit.exists():
                os.replace(kit, old)
                moved = True
            os.replace(new, kit)
        except OSError:
            if moved and not kit.exists():
                os.replace(old, kit)   # the game keeps the kit it had
            raise InstallError('kit.in_use', path=str(kit)) from None
        try:
            remove(old)
        except OSError as e:
            rep.log(f'kit: the old kit stays until the next run: {e}')
        rep.log(f'kit: {len(files)} files, {total / GiB:.2f} GiB in {kit}')
        return {'bytes': total}

    def settle(self, ctx):
        if not layouts.uses_kit(ctx.layout):
            return
        kit = layouts.kit_folder(ctx.dest).resolve()
        if ctx.layout.root.resolve() != kit:
            ctx.layout = layouts.release_layout(kit)
            ctx.reporter.log(f'kit: the next steps run from {kit}')


def adopt_old_temp(layout, dest, temp):
    """A setup that was cancelled or failed before releases kept their working state in the install folder has its temp
    folder (generated C, objects) in that setup's folder: <setup>/temp/<install name>-<hash>. Run again from the same
    folder, the setup moves it into the install folder when both are on one disk, and the steps' keys decide what of it
    still holds. On another disk the new temp folder starts empty.
    Runs before the run's log opens in temp; returns the line for the log, None when there was nothing to move."""
    if not layouts.uses_kit(layout) or temp != dest / layouts.TEMP_IN_DEST or temp.exists():
        return None
    old = layouts.temp_folder(layout.root / 'temp', dest)
    if not (old / 'gen').is_dir():
        return None
    try:
        temp.parent.mkdir(parents=True, exist_ok=True)
        os.replace(old, temp)
        return f'temp: moved {old} to {temp}'
    except OSError as e:
        return f'temp: {old} stays where it is ({e}); {temp} starts empty'


# ---- 2. extract

class Extract(Step):
    id = 'extract'

    def key(self, ctx):
        # The output is whatever matches the expected hashes, whichever files and tools produced it.
        return fingerprint('extract-1', ctx.layout.expected(game_version(ctx), game_region(ctx)))

    def current(self, ctx, record):
        # Another update file than the recorded one extracts again; the check then identifies it.
        update, known = getattr(ctx.options, 'update', None), ctx.state['inputs'].get('update')
        if update and known and Path(update).is_file():
            st = Path(update).stat()
            if (st.st_size, st.st_mtime_ns) != (known.get('size'), known.get('mtime_ns')) \
                    and file_sha256(Path(update)) != known.get('sha256'):
                return False
        expected = expected_update(ctx)
        romfs = ctx.dest / 'romfs'
        if extract.exefs_problems(ctx.dest / 'exefs', expected) or not extract.romfs_verified(romfs, expected):
            return False
        st = (romfs / 'romfs.bin').stat()
        return st.st_size == record.get('romfs_size') and st.st_mtime_ns == record.get('romfs_mtime_ns')

    def verify(self, ctx, record):
        # Called for another release's record. current() has already found romfs.bin with the size and modification
        # time recorded when the merge finished, and the merge hashed every byte it wrote against the expected hash.
        # Reading the 14 GB again would make every update wait minutes for nothing, so that happens only with
        # --verify (a disk suspected of damaging files).
        if not ctx.verify:
            return True
        image = ctx.dest / 'romfs' / 'romfs.bin'
        rep = ctx.reporter
        digest = file_sha256(image, lambda d, t: rep.progress('check', 'verify_romfs', d, t, 'bytes'))
        return digest == expected_update(ctx)['romfs']['sha256']

    def run(self, ctx):
        o, dest, rep = ctx.options, ctx.dest, ctx.reporter
        expected = expected_update(ctx)
        base, update, keys = Path(o.base), Path(o.update), Path(o.keys)
        inputs = {}
        for name, path in (('base', base), ('update', update)):
            known = ctx.state['inputs'].get(name, {})
            st = path.stat()
            if known.get('size') == st.st_size and known.get('mtime_ns') == st.st_mtime_ns and \
                    (known.get('sha256') or name == 'base'):
                inputs[name] = known
                continue
            if name == 'base':   # only the update's checksum is compared later (Extract.current); the base's 13 GB
                inputs[name] = {'name': path.name, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns}   # are not read
                continue
            digest = file_sha256(path, lambda d, t, n=name: rep.progress('extract', 'hash_' + n, d, t, 'bytes'))
            inputs[name] = {'name': path.name, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'sha256': digest}
        ctx.state['inputs'] = inputs
        ctx.save()

        dest.mkdir(parents=True, exist_ok=True)
        for stale in dest.glob('update-merge-*'):   # a killed merge leaves its NCA copies (and key copies) behind
            remove(stale)
        rep.progress('extract', 'exefs', force=True)
        staged = ctx.temp / 'check' / 'update' / 'exefs'
        if not staged.is_dir():
            remove(ctx.temp / 'check' / 'update')
            try:
                extract.extract_exefs(update, keys, ctx.temp / 'check' / 'update', ctx.temp / 'logs', update=True)
            except extract.DamagedFile as e:
                raise InstallError('check.input_damaged', input='update', path=str(e.path), detail=e.detail) from None
            except ValueError:
                raise InstallError('check.exefs_failed', input='update',
                                   log=str(ctx.temp / 'logs' / 'extract_exefs-update.log')) from None
        remove(dest / 'exefs')
        shutil.move(staged, dest / 'exefs')
        problems = extract.exefs_problems(dest / 'exefs', expected)
        if problems:
            raise InstallError('extract.exefs_mismatch', version=game_version(ctx), modules=', '.join(problems))
        if extract.sha256(dest / 'exefs' / 'main.npdm') != expected['exefs_sha256']['main.npdm']:
            rep.warning('warn.npdm_differs', version=game_version(ctx))

        romfs = dest / 'romfs'
        if self.id in ctx.forced:
            remove(romfs)   # the full hash failed: index.json still claims the extraction's hash
        if not extract.romfs_verified(romfs, expected):
            self.merge(ctx, base, update, keys, romfs, expected)
        report = json.loads((romfs / 'index.json').read_text())
        if report['romfs_size'] != expected['romfs']['size'] or report['romfs_sha256'] != expected['romfs']['sha256']:
            raise InstallError('extract.romfs_mismatch', version=game_version(ctx), size=report['romfs_size'],
                               sha256=report['romfs_sha256'][:16])
        (dest / 'profile').mkdir(exist_ok=True)
        st = (romfs / 'romfs.bin').stat()
        return {'romfs_size': st.st_size, 'romfs_mtime_ns': st.st_mtime_ns}

    def merge(self, ctx, base, update, keys, romfs, expected):
        # tools/xc2_merge_romfs.py reads both game files in place and writes the image once, hashing it as it goes. It
        # sizes the image up front, so the progress comes from its callback, not from the file's size.
        written = [0]
        watch = Watch(ctx.reporter, 'extract')
        watch.set('merge', lambda: written[0], expected['romfs']['size'])
        watch.start()
        try:
            extract.merge_update_romfs(base, update, keys, romfs,
                                       progress=lambda done, total: written.__setitem__(0, done))
        except extract.DamagedFile as e:
            raise InstallError('check.input_damaged', input='base' if e.path == base else 'update', path=str(e.path),
                               detail=e.detail) from None
        except (ValueError, OSError) as e:
            raise InstallError('extract.romfs_merge_failed', detail=str(e),
                               log=str(ctx.reporter.log_path or '')) from None
        finally:
            watch.stop()


# ---- 2b. add-on content

AOC_DATA = 5   # NCA content type of add-on content (PublicData)
# Add-on packs extracted at once. Each takes a core and, while it runs, room for its RomFS image beside the unpacked
# files (Check.space counts the DLC_JOBS largest).
DLC_JOBS = max(1, min(3, (os.cpu_count() or 2) // 2))


def dlc_inputs(ctx):
    """The add-on content files chosen (--dlc, any number), else none."""
    return [Path(p) for p in (getattr(ctx.options, 'dlc', None) or [])]


def dlc_record(f, path, keys, region):
    """(title id, NCA record) of the add-on content in an opened NSP. InstallError when it holds none of this release's
    (region, tools/xc2_titles.py): another release's add-on names it, since the game would not mount it."""
    try:
        kind, entries = container_entries(f, path.stat().st_size)
        records = nca_inventory(f, entries, keys)
    except ValueError as e:
        raise_if_damaged(e, 'dlc', path)
        raise InstallError('check.file_unreadable', input='dlc', detail=str(e)) from None
    for record, _ in records:
        if record['content_type'] == AOC_DATA and region.aoc_index(record['title_id']) is not None:
            return record['title_id'], record
    for record, _ in records:
        other = xc2_titles.of_aoc(record['title_id']) if record['content_type'] == AOC_DATA else None
        if other:
            raise InstallError('check.dlc_other_region', path=str(path), dlc=other[0].name, game=region.name)
    raise InstallError('check.dlc_not_game', path=str(path))


class Dlc(Step):
    """Extracts the RomFS of each add-on content file into aoc/<index>/, where xc2.exe mounts it (runtime/fs_host.c
    mount_aoc). Without --dlc it keeps what an earlier run extracted."""
    id = 'dlc'

    def key(self, ctx):
        files = dlc_inputs(ctx)
        if not files:   # nothing chosen: whatever is recorded stays current
            return ctx.state.get('steps', {}).get('dlc', {}).get('key') or fingerprint('dlc-1')
        # a missing file must reach the check (check.input_missing), not fail planning here
        return fingerprint('dlc-1', *(f'{p.name}:{p.stat().st_size}:{p.stat().st_mtime_ns}' if p.is_file()
                                      else f'{p}:missing' for p in files))

    def current(self, ctx, record):
        return all((ctx.dest / 'aoc' / str(i)).is_dir() for i in record.get('indices', []))

    def run(self, ctx):
        files, dest, rep = dlc_inputs(ctx), ctx.dest, ctx.reporter
        # packs an earlier run extracted stay installed (add-ons can be added a few at a time) and stay recorded, so
        # current() keeps checking them; a pack chosen again is extracted again
        before = [i for i in (ctx.state.get('steps', {}).get('dlc') or {}).get('indices', [])
                  if (dest / 'aoc' / str(i)).is_dir()]
        if not files:
            return {'indices': before}
        keys_path = Path(ctx.options.keys)
        keys = load_keys(keys_path)
        logs = ctx.temp / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        todo = []
        for path in files:
            with path.open('rb') as f:
                title_id, _ = dlc_record(f, path, keys, game_region(ctx))
            index = game_region(ctx).aoc_index(title_id)
            # unpacked under a name the game does not mount, then renamed: a cancelled or killed run leaves no
            # half-filled aoc/<index> that runtime/fs_host.c aoc_present would take for an installed pack
            part = dest / 'aoc' / f'.{index}.part'
            remove(part)
            part.parent.mkdir(parents=True, exist_ok=True)
            todo = [t for t in todo if t[2] != index]   # the same pack chosen twice: one process for it
            todo.append((path, title_id, index, part, logs / f'dlc-{index}.log'))
        del keys
        # XC2's add-on content is encrypted with the title key of the package's ticket, which hactool is not given:
        # tools/xc2_extract.py dlc_romfs reads the ticket, verifies every IVFC level and unpacks the files loose into
        # aoc/<index>/ as the runtime mounts them. Each pack runs in a process of its own (the verification is
        # Python-bound), DLC_JOBS at a time, with its output in its log.
        running, indices, waiting = [], [], list(todo)
        rep.progress('dlc', 'extract', 0, len(files), 'files')
        try:
            while waiting or running:
                while waiting and len(running) < DLC_JOBS:
                    path, title_id, index, part, log = item = waiting.pop(0)
                    out = open(log, 'w', encoding='utf-8', newline='\n')
                    command = [str(ctx.layout.python), '-c', extract.RUN_SCRIPT,
                               str(ctx.layout.tools / 'xc2_extract.py'), 'dlc-romfs', str(path), '--keys', str(keys_path), '--output', str(part)]
                    rep.log('run ' + ' '.join(command[3:]))
                    proc = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, cwd=ctx.layout.root,
                                            env=dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8'),
                                            start_new_session=os.name != 'nt')
                    running.append((proc, out, item))
                time.sleep(0.2)
                for entry in [e for e in running if e[0].poll() is not None]:
                    running.remove(entry)
                    proc, out, (path, title_id, index, part, log) = entry
                    out.close()
                    if proc.returncode:
                        remove(part)
                        damage = extract.DAMAGE.search(log.read_text(encoding='utf-8', errors='replace'))
                        if damage:
                            raise InstallError('check.input_damaged', input='dlc', path=str(path),
                                               detail=damage.group(1).strip())
                        raise InstallError('dlc.extract_failed', path=str(path), log=str(log))
                    out_dir = dest / 'aoc' / str(index)
                    remove(out_dir)
                    os.replace(part, out_dir)
                    rep.log(f'dlc: {path.name} is add-on content {index} ({title_id})')
                    indices.append(index)
                    rep.progress('dlc', 'extract', len(indices), len(files), 'files')
        except BaseException:
            for proc, out, (_, _, _, part, _) in running:
                kill_tree(proc)
                proc.wait()
                out.close()
                remove(part)
            raise
        rep.progress('dlc', 'extract', len(files), len(files), 'files')
        return {'indices': sorted(set(before) | set(indices))}


# ---- 3. lift

EMITTED = re.compile(r'^emitted (\d+)/(\d+) chunks')
DISCOVERED = re.compile(r'^(\w+): \d+ functions \(.*discovery')


class Lift(Step):
    id = 'lift'
    transient = True

    def key(self, ctx):
        # xc2_titles.py names the release's patch table, so a change there has to lift the game again
        return fingerprint('lift-5', ctx.keys['extract'], ctx.layout.recomp, ctx.layout.tools / 'xc2_titles.py')

    def current(self, ctx, record):
        gen = ctx.temp / 'gen'
        stamp = gen / STAMP
        return (gen / 'layout.json').is_file() and stamp.is_file() and stamp.read_text().strip() == ctx.keys['lift']

    def run(self, ctx):
        # a gen/ an unfinished run left stays: gen.py lifts again only the chunks whose inputs changed
        # (gen/<mod>/lift_cache.json; XC2_LIFT_CACHE=0 lifts all) and rewrites only those whose C changed, so the compile
        # step compiles only those. The player's mods are not in it: the game builds their code when it starts
        # (tools/mod_code.py).
        layout, rep, gen = ctx.layout, ctx.reporter, ctx.temp / 'gen'
        (gen / STAMP).unlink(missing_ok=True)
        rep.progress('lift', 'discover', force=True)

        def parse(line):
            m = EMITTED.match(line)
            if m:
                rep.progress('lift', 'emit', int(m.group(1)), int(m.group(2)), 'chunks')

        args = ['--exefs', ctx.dest / 'exefs', '--out', gen]
        # An update without a table is built without code patches: the runtime's frame rate and resolution code that
        # reads main's data switches on with the patch sets only, so the game runs as on the console.
        table = layout.recomp / game_region(ctx).patch_table(game_version(ctx))
        if table.is_file():   # code patch sites the runtime switches (XC2_FPS60 and the like)
            args += ['--patches', table]
        code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args, parse)
        if code:
            raise InstallError('lift.failed', code=code, log=str(log))
        (gen / STAMP).write_text(ctx.keys['lift'] + '\n')
        return {}


# ---- 4. compile

NINJA_LINE = re.compile(r'^\[(\d+)/(\d+)\] ')


def object_cache(ctx):
    """build-cache/objects: the generated objects tools/build_game.py keeps between builds (ObjectCache). Its keys hold
    no folder, unlike Ninja's commands, so an update with a newer release compiles only the chunks whose C or headers
    changed."""
    return ctx.dest / 'build-cache' / 'objects'


def mod_base(folder):
    """The folder tools/build_game.py leaves beside the exe for the mods' code (tools/mod_abi.py)."""
    return folder / 'build-cache' / 'mod-base'


class Compile(Step):
    id = 'compile'

    def key(self, ctx):
        layout = ctx.layout
        # a missing compiler is the check's check.release_incomplete, not a planning crash
        # without the lines that name clang's folder: the build kit is the same toolchain as the setup's
        clang = '\n'.join(line for line in subprocess.run([str(layout.clang), '--version'], capture_output=True,
                                                          text=True).stdout.splitlines()
                          if not line.startswith(('InstalledDir:', 'Configuration file:'))) \
            if layout.clang.is_file() else 'missing'
        tools = [layout.tools / n for n in ('build_game.py', 'build_shaders.py', 'romfs_index.py', 'check_content.py',
                                              'mod_abi.py')]
        return fingerprint('compile-2', ctx.keys['lift'], layout.runtime, layout.runtime_lib, layout.deps or 'system',
                           layout.sysroot or 'host', *tools, clang, *[d.name for d in layout.dlls])

    def current(self, ctx, record):
        exe = ctx.dest / GAME_EXE
        if not exe.is_file() or not (ctx.dest / 'romfs-index.bin').is_file() or not mod_base(ctx.dest).is_dir():
            return False
        if not all((ctx.dest / d.name).is_file() for d in ctx.layout.dlls):
            return False
        return file_sha256(exe) == record.get('exe_sha256')

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        build = ctx.temp / 'build'
        phase = ['compile']

        def parse(line):
            m = NINJA_LINE.match(line)
            if m:
                rep.progress('compile', phase[0], int(m.group(1)), int(m.group(2)), 'objects')
            elif line.startswith('compile: '):
                phase[0] = 'link'
                rep.progress('compile', 'link', force=True)

        rep.progress('compile', 'compile', force=True)
        code, log = run_tool(ctx, 'compile', layout.tools / 'build_game.py', [
            '--gen', ctx.temp / 'gen', '--out', build, '--toolchain', layout.llvm,
            *(['--deps', layout.deps] if layout.deps else []), *(['--sysroot', layout.sysroot] if layout.sysroot else []),
            '--runtime', layout.runtime, '--runtime-lib', layout.runtime_lib, '--ninja', layout.ninja,
            '--version', version_tag(game_version(ctx), game_region(ctx)),
            '--expected', layout.expected(game_version(ctx), game_region(ctx)),
            '--exefs', dest / 'exefs', '--romfs', dest / 'romfs',
            '--profile', dest / 'profile', '--object-cache', object_cache(ctx)], parse)
        exe = build / ('xc2' + EXE)
        if code or not exe.is_file():
            raise InstallError('compile.failed', code=code, log=str(log))
        deadline = time.monotonic() + EXE_WAIT
        while True:
            try:
                replace_file(exe, dest / GAME_EXE)
                break
            except PermissionError:   # the game is still closing
                if time.monotonic() > deadline:
                    raise InstallError('compile.exe_in_use', path=str(dest / GAME_EXE)) from None
                time.sleep(0.5)
        replace_file(build / 'romfs-index.bin', dest / 'romfs-index.bin')
        # what the mods' code needs of this lift belongs to this exe (tools/mod_abi.py)
        remove(mod_base(dest))
        if mod_base(build).is_dir():
            # copied whole before it takes its name: a copy that stops half way must not count as made (current)
            part = mod_base(dest).with_name('mod-base.part')
            remove(part)
            mod_base(dest).parent.mkdir(exist_ok=True)
            shutil.copytree(mod_base(build), part)
            os.replace(part, mod_base(dest))
        for dll in layout.dlls:
            replace_file(dll, dest / dll.name)
        return {'exe_sha256': file_sha256(dest / GAME_EXE)}


# ---- 5. prepare

class Prepare(Step):
    """Puts the shader translator beside xc2.exe and draws the Xbox, PlayStation and keyboard button prompts from the
    player's archive into prompts/ (tools/xc2_prompts.py, runtime/prompts.c). XC2 has no shader list to translate
    ahead of play yet: shaders are translated on first use and cached in shader-cache/."""
    id = 'prepare'

    def key(self, ctx):
        layout = ctx.layout
        # the RomFS the prompts are drawn from, not the compiled game: a newer exe does not draw them again
        return fingerprint('prepare-4', ctx.keys['extract'], layout.xlate, layout.xlate.with_suffix('.id'),
                           layout.tools / 'xc2_prompts.py', layout.tools / 'data' / 'kenney-input-prompts')

    def current(self, ctx, record):
        xlate = ctx.dest / XLATE
        return (xlate.is_file() and file_sha256(xlate) == record.get('xlate_sha256') and
                (ctx.dest / 'prompts' / 'manifest.tsv').is_file())

    def run(self, ctx):
        layout, dest = ctx.layout, ctx.dest
        ctx.reporter.progress('prepare', 'translator', force=True)
        xlate = dest / XLATE
        if xlate.is_file() and file_sha256(xlate) != file_sha256(layout.xlate):
            remove(dest / 'shader-cache')   # another translator: its cache keys differ, the old ones only take space
        replace_file(layout.xlate, xlate)
        xlate_id = layout.xlate.with_suffix('.id')
        if xlate_id.is_file():
            replace_file(xlate_id, xlate.with_suffix('.id'))
        else:
            remove(xlate.with_suffix('.id'))
        # the prompts are optional: without them the game shows its own, so a failure is logged, not fatal
        ctx.reporter.progress('prepare', 'prompts', force=True)
        remove(dest / 'prompts')
        code, log = run_tool(ctx, 'prepare', layout.tools / 'xc2_prompts.py',
                             ['--romfs', dest / 'romfs', '--out', dest / 'prompts'])
        if code:
            remove(dest / 'prompts')
            ctx.reporter.log(f'xc2_prompts.py failed (exit code {code}); the game keeps its own button prompts. {log}')
        return {'xlate_sha256': file_sha256(xlate)}


# ---- 6. clean up

class Cleanup(Step):
    id = 'cleanup'
    always = True

    def run(self, ctx):
        rep, dest = ctx.reporter, ctx.dest
        rep.progress('cleanup', 'ini', force=True)
        # the installed RomFS has the update applied: its languages are the ones the game runs with, which the in-game
        # menu offers and the runtime keeps to (game_langs, runtime/ov_system.c ov_language_shipped)
        langs = installed_languages(dest) or ctx.languages
        try:
            ini.write(dest, ctx.lang, ctx.layout.ini_example, game_langs=langs)
        except (OSError, ValueError) as e:   # ValueError: an xc2.ini saved in another encoding than UTF-8
            raise InstallError('cleanup.ini_failed', detail=str(e)) from None
        ctx.state['lang'] = ctx.lang
        if langs:
            ctx.state['languages'] = list(langs)
        (dest / 'profile').mkdir(exist_ok=True)
        if (dest / GAME_EXE).is_file():   # an install from before the rename keeps the old exe until the new one is in
            try:
                remove(dest / LEGACY_EXE)
            except OSError:   # still running: the next run tries again (cleanup runs every time)
                pass
        rep.progress('cleanup', 'temp', force=True)
        for stale in dest.glob('update-merge-*'):
            remove(stale)
        for name in ('gen', 'build', 'check', 'dlc'):
            remove(ctx.temp / name)
        write_mod_command(ctx)
        return {}


def mod_command(ctx):
    """The command the game runs at start to build the code of new or changed mods (runtime/modcode.c), without the
    arguments the game adds (--exefs, --mods, --abi). The build kit's paths are relative to the install folder, which is
    the command's working folder, so a moved install folder keeps it.
    In the Linux AppImage the release folder is the AppImage's mount (/tmp/.mount_*), which goes away when the setup
    exits, so a path into it would fail to start later. The command starts the AppImage file itself ($APPIMAGE, set by
    the AppImage runtime), whose AppRun passes the arguments to installer.gui."""
    layout = ctx.layout
    appimage = os.environ.get('APPIMAGE', '')
    if not layout.dev and appimage and Path(appimage).is_file():
        return [str(Path(appimage).resolve()), '--mod-code', '--cache', 'build-cache']
    root, base = layout.root, layout.root
    if layouts.uses_kit(layout) and root.resolve() == layouts.kit_folder(ctx.dest).resolve():
        base = Path(layouts.KIT)

    def at(path):   # a path of the release folder, below base
        path = Path(path)
        return str(base / path.relative_to(root)) if path.is_relative_to(root) else str(path)
    python = Path(sys.executable) if layout.dev else layout.python
    return [at(python), at(layout.tools / 'mod_code.py'), '--cache', 'build-cache', '--runtime', at(layout.runtime),
            '--clang', at(layout.clang), *(['--sysroot', at(layout.sysroot)] if layout.sysroot else [])]


def write_mod_command(ctx):
    """build-cache/modcode.txt in the install folder: mod_command, one argument per line."""
    cache = ctx.dest / 'build-cache'
    cache.mkdir(exist_ok=True)
    tmp = cache / 'modcode.txt.tmp'
    tmp.write_text('\n'.join(mod_command(ctx)) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, cache / 'modcode.txt')


def all_steps():
    return [Check(), Kit(), Extract(), Dlc(), Lift(), Compile(), Prepare(), Cleanup()]
