"""A newer setup release from GitHub: the setup window's update banner (docs/installer.md, "Updating the port").

    latest(repo, current)   the newest published release when its version is above current, else None
    Check                   latest() in a thread, so the window opens without waiting for the network
    Install                 a thread that downloads this platform's setup file of a release, checks it against the
                            SHA-256 GitHub lists for that file and puts it in place: the Windows zip unpacked into a
                            folder beside the running setup, the Linux AppImage beside the running one, marked
                            executable. Its result is the new setup
    launch(setup, dest)     starts the new setup's window, which updates dest at once when it holds an installation

Standard library only. The check is one unauthenticated request to api.github.com whose User-Agent names the setup and
its version, nothing else. A release without a file for this platform or without a SHA-256 is offered as a link to its
page only. RecompLauncher runs the CLI (installer/cli.py) and updates the release itself; this never runs there.
"""
import hashlib
import http.client
import json
import os
import re
import shutil
import ssl
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

REPO = 'AksharDP/xc2-recomp'   # recomplauncher.json's "repo" wins, so the release of a fork checks the fork
API = 'https://api.github.com/repos/{repo}/releases/latest'
TIMEOUT = 20                    # seconds for the connection and for each read: a stalled download fails, not hangs
CHUNK = 1 << 20
MAX_ANSWER = 1 << 20            # a releases/latest answer is a few KB; more is not GitHub's
PARTIAL = '.xc2-setup-partial'  # marks a folder this module is unpacking into; only such a folder is ever removed
ASSET_SUFFIX = {'windows': '-windows.zip', 'linux': '-linux.AppImage'}   # tools/make_release*.py, release.yml
REPO_RE = re.compile(r'^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')
SHA256_RE = re.compile(r'^sha256:([0-9a-f]{64})$')
# Where Linux distributions keep their CA bundle, for an OpenSSL (python-build-standalone's) that looks elsewhere
CA_FILES = ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt', '/etc/ssl/ca-bundle.pem',
            '/etc/ssl/cert.pem')
# Variables this process set for its own Tcl/Tk (gui.py fix_tcl_paths, AppRun) or the AppImage runtime set for it;
# the new setup must find its own
CHILD_DROP = ('TCL_LIBRARY', 'TK_LIBRARY', 'APPDIR', 'APPIMAGE', 'ARGV0')
WINDOWS_SETUP_PYTHON = ('python', 'pythonw.exe')


class UpdateError(Exception):
    """A failure the window shows: a catalog id (installer/text/*.json, ui.update_err_*) and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


class Cancelled(Exception):
    pass


# ---- versions and releases ---------------------------------------------------------------------------------------

def version_tuple(text):
    """(0, 1, 13) of 'v0.1.13', '0.1.13+973c626' (a release's VERSION) or '0.1.13-dev+973c626'; None for anything
    else."""
    core = str(text).strip().removeprefix('v').split('+')[0].split('-')[0]
    parts = core.split('.')
    return tuple(int(p) for p in parts) if all(p.isdigit() for p in parts) else None


def is_newer(candidate, current):
    a, b = version_tuple(candidate), version_tuple(current)
    return a is not None and b is not None and a > b


def platform():
    """'windows', 'linux', or None where no release is published."""
    if os.name == 'nt':
        return 'windows'
    return 'linux' if sys.platform.startswith('linux') else None


def repo_of(root):
    """The GitHub repository the setup at root comes from: recomplauncher.json's "repo" (the release ships it at its
    top), else REPO."""
    try:
        repo = json.loads((Path(root) / 'recomplauncher.json').read_text(encoding='utf-8')).get('repo')
    except (OSError, ValueError, AttributeError):
        return REPO
    return repo if isinstance(repo, str) and REPO_RE.match(repo) else REPO


@dataclass(frozen=True)
class Release:
    version: str          # '0.1.14': the tag without its v
    page: str             # the release's page on GitHub
    asset: str = ''       # this platform's setup file; '' when the release has none
    url: str = ''
    size: int = 0
    sha256: str = ''

    @property
    def installable(self):
        """True when the setup file can be fetched and checked: a URL, a size and the SHA-256 GitHub computed."""
        return bool(self.url and self.sha256 and self.size > 0)


def parse_release(data, system):
    """The Release of a releases/latest answer for system ('windows', 'linux'), or None when it is not a published
    release with a version tag."""
    if not isinstance(data, dict) or data.get('draft') or data.get('prerelease'):
        return None
    tag, page = data.get('tag_name'), data.get('html_url')
    if not isinstance(tag, str) or version_tuple(tag) is None:
        return None
    if not isinstance(page, str) or not page.startswith('https://github.com/'):
        return None
    version = tag.strip().removeprefix('v')
    suffix = ASSET_SUFFIX.get(system)
    for asset in data.get('assets') or ():
        if suffix and isinstance(asset, dict) and asset.get('name') == f'XC2-Setup-{version}{suffix}':
            url, size = asset.get('browser_download_url'), asset.get('size')
            digest = SHA256_RE.match(str(asset.get('digest') or ''))
            if isinstance(url, str) and url.startswith('https://github.com/') and isinstance(size, int) and digest:
                return Release(version, page, asset['name'], url, size, digest.group(1))
            break
    return Release(version, page)


def tls_context():
    """The system's certificate authorities: Windows' store, else OpenSSL's default paths plus the distribution's
    bundle when those held none."""
    context = ssl.create_default_context()
    if os.name != 'nt' and not context.cert_store_stats().get('x509_ca'):
        for path in CA_FILES:
            if os.path.isfile(path):
                try:
                    context.load_verify_locations(cafile=path)
                    break
                except (OSError, ssl.SSLError):
                    continue
    return context


def opener_for(current):
    """open(url, accept) -> a response, the way every request of this module is made. Tests pass their own."""
    agent = f'XC2-Setup/{current}'

    def open_url(url, accept):
        request = urllib.request.Request(url, headers={'User-Agent': agent, 'Accept': accept})
        return urllib.request.urlopen(request, timeout=TIMEOUT, context=tls_context())
    return open_url


def latest(repo, current, system=None, opener=None):
    """The newest published release of repo when it is newer than current, else None. No connection, GitHub's rate
    limit and an answer that is not a release are None too: the check never stands in the setup's way."""
    try:
        with (opener or opener_for(current))(API.format(repo=repo), 'application/vnd.github+json') as response:
            data = json.loads(response.read(MAX_ANSWER).decode('utf-8'))   # a longer answer is cut and fails here
    except (OSError, ValueError, http.client.HTTPException):
        return None
    release = parse_release(data, system or platform())
    return release if release and is_newer(release.version, current) else None


class Check:
    """latest() in a thread. done() is True once result holds its answer."""

    def __init__(self, repo, current, system=None, opener=None):
        self.result = None
        self.finished = threading.Event()
        self.thread = threading.Thread(target=self.run, args=(repo, current, system, opener), daemon=True)
        self.thread.start()

    def run(self, repo, current, system, opener):
        try:
            self.result = latest(repo, current, system, opener)
        finally:
            self.finished.set()

    def done(self):
        return self.finished.is_set()


# ---- putting the new setup in place --------------------------------------------------------------------------------

def can_install(root, system=None):
    """True when this setup can put a newer one in place: an unpacked Windows release folder with its XC2-Setup.exe
    (neither a checkout nor the build kit in an install folder, which has no XC2-Setup.exe: installer/layout.py
    KIT_ITEMS), or an AppImage (the runtime names the file in APPIMAGE)."""
    system = system or platform()
    if system == 'windows':
        return Path(root).joinpath(*WINDOWS_SETUP_PYTHON).is_file() and (Path(root) / 'XC2-Setup.exe').is_file()
    if system == 'linux':
        image = os.environ.get('APPIMAGE')
        return bool(image) and Path(image).is_file()
    return False


def places(root, system=None):
    """The folders a new setup may go to, in order: beside the running one (on Linux beside its AppImage file), then a
    per-user folder for a setup unpacked somewhere the player cannot write."""
    if (system or platform()) == 'windows':
        base = os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA')
        user = (Path(base) if base else Path.home() / 'AppData' / 'Local') / 'XC2 Recomp' / 'setup'
        return [Path(root).parent, user]
    image, data = os.environ.get('APPIMAGE'), os.environ.get('XDG_DATA_HOME')
    user = (Path(data) if data else Path.home() / '.local' / 'share') / 'xc2-recomp' / 'setup'
    return ([Path(image).parent] if image else []) + [user]


def is_setup(folder, version):
    """True when folder holds an unpacked Windows setup whose VERSION is version (build metadata aside)."""
    try:
        text = (Path(folder) / 'VERSION').read_text(encoding='utf-8')
    except (OSError, UnicodeError):
        return False
    return Path(folder).joinpath(*WINDOWS_SETUP_PYTHON).is_file() and version_tuple(text) == version_tuple(version)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(CHUNK), b''):
            h.update(block)
    return h.hexdigest()


def need_space(folder, need):
    free = shutil.disk_usage(folder).free
    if free < need:
        raise UpdateError('ui.update_err_space', path=str(folder), need=f'{need / 1e9:.1f}', free=f'{free / 1e9:.1f}')


def unpack(archive, folder, progress=None, cancelled=None):
    """Extract a release zip into folder, without the one top folder make_release.py write_zip puts every file under
    (XC2-Setup/). A name that would land outside folder stops it before anything is written."""
    try:
        z = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile) as e:
        raise UpdateError('ui.update_err_unpack', detail=str(e)) from e
    with z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        names = [PurePosixPath(i.filename) for i in infos]
        strip = bool(names) and all(len(n.parts) > 1 for n in names) and len({n.parts[0] for n in names}) == 1
        base = Path(folder).resolve()
        targets = []
        for info, name in zip(infos, names):
            parts = name.parts[1:] if strip else name.parts
            if (not parts or name.is_absolute() or '\\' in info.filename
                    or any(p in ('.', '..') or ':' in p for p in parts)):
                raise UpdateError('ui.update_err_unpack', detail=info.filename)
            target = base.joinpath(*parts)
            if not target.resolve().is_relative_to(base):
                raise UpdateError('ui.update_err_unpack', detail=info.filename)
            targets.append(target)
        total = sum(i.file_size for i in infos)
        need_space(base, total)
        done = 0
        try:
            for info, target in zip(infos, targets):
                if cancelled and cancelled():
                    raise Cancelled()
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(target, 'wb') as dst:   # z.open checks each file's CRC at its end
                    shutil.copyfileobj(src, dst, CHUNK)
                done += info.file_size
                if progress:
                    progress(done, total)
        except (OSError, zipfile.BadZipFile, EOFError) as e:
            raise UpdateError('ui.update_err_unpack', detail=str(e)) from e


def rename_folder(src, dst, tries=40, wait=0.25):
    """os.rename, tried again for up to about 10 s: a virus scanner often holds a just-written exe or DLL open for a
    moment, and Windows refuses to rename a folder with an open file in it meanwhile."""
    for attempt in range(tries):
        try:
            os.rename(src, dst)
            return
        except PermissionError:
            if attempt == tries - 1:
                raise
            time.sleep(wait)


def remove_partial(folder):
    """Delete a folder this module was unpacking into: only when it carries the PARTIAL marker."""
    if (Path(folder) / PARTIAL).is_file():
        shutil.rmtree(folder, ignore_errors=True)


class Install:
    """Downloads, checks and places a release's setup file in a thread. While it runs the window reads phase
    ('download', 'unpack', 'done'), done_bytes and total; once done(), result is the new setup (a folder on Windows,
    the AppImage on Linux) or error is set (an UpdateError, or Cancelled after cancel())."""

    def __init__(self, release, folders, system=None, opener=None):
        self.release, self.folders = release, [Path(p) for p in folders]
        self.system = system or platform()
        self.opener = opener or opener_for('update')
        self.phase, self.done_bytes, self.total = 'download', 0, release.size
        self.result = self.error = None
        self.cancel_event, self.finished = threading.Event(), threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def cancel(self):
        self.cancel_event.set()

    def done(self):
        return self.finished.is_set()

    def run(self):
        try:
            self.result = self.place_windows() if self.system == 'windows' else self.place_appimage()
            self.phase = 'done'
        except (UpdateError, Cancelled) as e:
            self.error = e
        except Exception as e:   # a bug here must still end the job, or the window waits for ever
            self.error = UpdateError('ui.update_err_other', detail=f'{type(e).__name__}: {e}')
        finally:
            self.finished.set()

    def progress(self, done, total):
        self.done_bytes, self.total = done, total

    def download(self, path):
        """The release file into path, hashed while it arrives; a short, long or different file is refused."""
        r = self.release
        h = hashlib.sha256()
        self.progress(0, r.size)
        try:
            with self.opener(r.url, 'application/octet-stream') as response, open(path, 'wb') as out:
                while True:
                    if self.cancel_event.is_set():
                        raise Cancelled()
                    block = response.read(CHUNK)
                    if not block:
                        break
                    if self.done_bytes + len(block) > r.size:
                        raise UpdateError('ui.update_err_damaged')
                    out.write(block)
                    h.update(block)
                    self.progress(self.done_bytes + len(block), r.size)
        except (OSError, http.client.HTTPException) as e:
            raise UpdateError('ui.update_err_download', detail=str(e) or type(e).__name__) from e
        if self.done_bytes != r.size or h.hexdigest() != r.sha256:
            raise UpdateError('ui.update_err_damaged')

    def place_windows(self):
        """XC2-Setup-<version>\\ beside the running setup (else in the per-user folder), unpacked from the zip in a
        marked .partial folder first, so an interrupted update never leaves a half setup under the final name. A setup
        of that version already there (an earlier update, or unpacked by hand) is used as it is."""
        r, name = self.release, f'XC2-Setup-{self.release.version}'
        for parent in self.folders:
            if is_setup(parent / name, r.version) and not (parent / name / PARTIAL).exists():
                return parent / name
            staging = parent / (name + '.partial')
            try:
                remove_partial(staging)
                staging.mkdir(parents=True)
                (staging / PARTIAL).write_bytes(b'')
            except OSError:
                continue   # not writable, or a folder of that name that is not ours: the next place
            break
        else:
            raise UpdateError('ui.update_err_write', path=str(self.folders[0]) if self.folders else '')
        archive = parent / (r.asset + '.part')
        try:
            need_space(parent, r.size)
            self.download(archive)
            self.progress(0, 0)   # before the phase: the window never shows the unpack with the download's figures
            self.phase = 'unpack'
            unpack(archive, staging, self.progress, self.cancel_event.is_set)
            if not is_setup(staging, r.version):
                raise UpdateError('ui.update_err_unpack', detail=f'no setup of {r.version} in {r.asset}')
            final, n = parent / name, 2
            while final.exists():   # a folder of that name that is not this setup: XC2-Setup-<version>-2, ...
                final, n = parent / f'{name}-{n}', n + 1
            (staging / PARTIAL).unlink()
            try:
                rename_folder(staging, final)
            except OSError as e:
                (staging / PARTIAL).write_bytes(b'')
                raise UpdateError('ui.update_err_unpack', detail=str(e)) from e
            return final
        finally:
            archive.unlink(missing_ok=True)
            remove_partial(staging)

    def place_appimage(self):
        """XC2-Setup-<version>-linux.AppImage beside the running AppImage (else in the per-user folder), downloaded to a
        .part file and renamed once checked. That file already there with the release's hash is used as it is."""
        r = self.release
        for folder in self.folders:
            final, part = folder / r.asset, folder / (r.asset + '.part')
            if final.is_file() and file_sha256(final) == r.sha256:
                final.chmod(0o755)
                return final
            try:
                folder.mkdir(parents=True, exist_ok=True)
                part.open('wb').close()
            except OSError:
                continue
            break
        else:
            raise UpdateError('ui.update_err_write', path=str(self.folders[0]) if self.folders else '')
        try:
            need_space(folder, r.size)
            self.download(part)
            part.chmod(0o755)
            os.replace(part, final)
            return final
        finally:
            part.unlink(missing_ok=True)


# ---- starting it -------------------------------------------------------------------------------------------------

def launch_command(setup, dest, system=None):
    """The new setup's window: --update-install DEST makes it update that installation at once (gui.py main)."""
    args = ['--update-install', str(dest)] if dest else []
    if (system or platform()) == 'windows':
        return [str(Path(setup).joinpath(*WINDOWS_SETUP_PYTHON)), '-m', 'installer.gui', *args]
    return [str(setup), *args]


def child_env(environ=None):
    """The environment for the new setup: this one without CHILD_DROP."""
    return {k: v for k, v in (os.environ if environ is None else environ).items() if k.upper() not in CHILD_DROP}


def launch(setup, dest, system=None):
    """Start the new setup on its own; the caller closes its window once it runs. Windows starts the new folder's
    pythonw.exe in that folder, as its XC2-Setup.exe does (installer/launcher/launcher.c)."""
    system = system or platform()
    common = dict(env=child_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                  close_fds=True)
    if system == 'windows':
        return subprocess.Popen(launch_command(setup, dest, system), cwd=str(setup), **common)
    return subprocess.Popen(launch_command(setup, dest, system), cwd=str(Path(setup).parent),
                            start_new_session=True, **common)
