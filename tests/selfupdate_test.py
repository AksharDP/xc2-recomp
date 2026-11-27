"""installer/selfupdate.py: the release check, the download and its checks, the unpack and the start of the new setup.
No network: every request goes to a stand-in opener that serves bytes made here. Standard library only.

    .venv-py314\\Scripts\\python tests\\selfupdate_test.py
"""
import hashlib
import io
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import preferences  # noqa: E402
from installer import selfupdate as su  # noqa: E402

PAGE = 'https://github.com/AksharDP/xc2-recomp/releases/tag/v9.9.9'
DOWNLOAD = 'https://github.com/AksharDP/xc2-recomp/releases/download/v9.9.9/'


def answer(tag='v9.9.9', assets=None, **extra):
    """A releases/latest answer as GitHub sends it, cut to the fields the module reads."""
    return {'tag_name': tag, 'html_url': PAGE, 'draft': False, 'prerelease': False,
            'assets': assets if assets is not None else [], **extra}


def asset(name, data, digest=None):
    return {'name': name, 'size': len(data), 'browser_download_url': DOWNLOAD + name,
            'digest': digest if digest is not None else 'sha256:' + hashlib.sha256(data).hexdigest()}


def setup_zip(version='9.9.9+abc1234', top='XC2-Setup', extra=()):
    """A release zip the way make_release.py write_zip lays it out: every file under one top folder."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        files = {'VERSION': version + '\n', 'python/pythonw.exe': 'MZ', 'installer/__init__.py': '',
                 'recomplauncher.json': '{}'}
        for name, text in files.items():
            z.writestr(f'{top}/{name}' if top else name, text)
        for name, text in extra:
            z.writestr(name, text)
    return buf.getvalue()


class Response(io.BytesIO):
    """A urlopen result: a readable body that works as a context manager. gate: the first read waits for it."""

    def __init__(self, data, gate=None):
        super().__init__(data)
        self.gate = gate

    def read(self, n=-1):
        if self.gate is not None:
            self.gate.wait(10)
            self.gate = None
        return super().read(n)


class Opener:
    """Serves URL -> bytes (or an exception to raise) and records (url, accept) of each request."""

    def __init__(self, routes, gate=None):
        self.routes, self.gate, self.requests = routes, gate, []

    def __call__(self, url, accept):
        self.requests.append((url, accept))
        body = self.routes[url]
        if isinstance(body, Exception):
            raise body
        return Response(body, self.gate)


def release_for(data, name='XC2-Setup-9.9.9-windows.zip', system='windows', digest=None):
    return su.parse_release(answer(assets=[asset(name, data, digest)]), system)


def run(job):
    job.thread.join(30)
    assert job.done(), 'the install thread did not end'
    return job


class VersionTest(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(su.version_tuple('v0.1.13'), (0, 1, 13))
        self.assertEqual(su.version_tuple('0.1.13+973c626'), (0, 1, 13))
        self.assertEqual(su.version_tuple('0.1.13-dev+973c626'), (0, 1, 13))
        for text in ('', 'v', 'latest', '0.1.x', '1..2', None):
            self.assertIsNone(su.version_tuple(text), text)

    def test_newer(self):
        self.assertTrue(su.is_newer('v0.1.14', '0.1.13+973c626'))
        self.assertTrue(su.is_newer('0.2.0', '0.1.13'))
        self.assertTrue(su.is_newer('0.1.10', '0.1.9'))   # numbers, not text
        self.assertFalse(su.is_newer('v0.1.13', '0.1.13+973c626'))   # the same release: build metadata aside
        self.assertFalse(su.is_newer('v0.1.12', '0.1.13'))
        self.assertFalse(su.is_newer('nightly', '0.1.13'))
        self.assertFalse(su.is_newer('0.1.14', 'unknown'))   # a release folder without VERSION never updates

    def test_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(su.repo_of(tmp), su.REPO)   # no recomplauncher.json
            (Path(tmp) / 'recomplauncher.json').write_text('{"repo": "someone/fork"}', encoding='utf-8')
            self.assertEqual(su.repo_of(tmp), 'someone/fork')
            for bad in ('{"repo": "../../x"}', '{"repo": 5}', '[1]', 'not json'):
                (Path(tmp) / 'recomplauncher.json').write_text(bad, encoding='utf-8')
                self.assertEqual(su.repo_of(tmp), su.REPO, bad)
        self.assertEqual(su.repo_of(ROOT), su.REPO, 'REPO and recomplauncher.json name different repositories')


class ReleaseTest(unittest.TestCase):
    def test_this_platforms_file(self):
        win, lin = b'windows zip', b'linux appimage'
        data = answer(assets=[asset('XC2-Setup-9.9.9-linux.AppImage', lin), asset('XC2-Setup-9.9.9-windows.zip', win)])
        r = su.parse_release(data, 'windows')
        self.assertEqual((r.version, r.page, r.asset, r.size), ('9.9.9', PAGE, 'XC2-Setup-9.9.9-windows.zip', len(win)))
        self.assertEqual(r.sha256, hashlib.sha256(win).hexdigest())
        self.assertTrue(r.installable)
        self.assertEqual(su.parse_release(data, 'linux').asset, 'XC2-Setup-9.9.9-linux.AppImage')
        self.assertFalse(su.parse_release(data, None).installable)   # no release file for this system

    def test_only_a_link_without_a_checked_file(self):
        data = b'zip'
        for assets in ([asset('XC2-Setup-9.9.9-windows.zip', data, digest='')],             # no SHA-256
                       [asset('XC2-Setup-9.9.9-windows.zip', data, digest='md5:00')],
                       [asset('XC2-Setup-9.9.8-windows.zip', data)],                         # another version's
                       [dict(asset('XC2-Setup-9.9.9-windows.zip', data), browser_download_url='https://x.example/a')],
                       []):
            r = su.parse_release(answer(assets=assets), 'windows')
            self.assertEqual((r.version, r.page), ('9.9.9', PAGE))
            self.assertFalse(r.installable, assets)

    def test_not_a_release(self):
        for data in (answer(draft=True), answer(prerelease=True), answer(tag='nightly'), answer(tag=None),
                     dict(answer(), html_url='https://example.com/x'), [], None, 'x'):
            self.assertIsNone(su.parse_release(data, 'windows'), data)

    def test_latest(self):
        url = su.API.format(repo=su.REPO)
        opener = Opener({url: json.dumps(answer(tag='v0.1.14')).encode()})
        r = su.latest(su.REPO, '0.1.13+973c626', 'windows', opener)
        self.assertEqual(r.version, '0.1.14')
        self.assertEqual(opener.requests, [(url, 'application/vnd.github+json')])
        self.assertIsNone(su.latest(su.REPO, '0.1.14+abc', 'windows', opener))   # nothing newer
        self.assertIsNone(su.latest(su.REPO, '0.1.15', 'windows', opener))

    def test_latest_never_fails(self):
        url = su.API.format(repo=su.REPO)
        for body in (urllib.error.URLError('offline'), TimeoutError('slow'), b'<html>', b'{"message": "rate limit"}',
                     b'\xff\xfe', b'{' * (su.MAX_ANSWER + 10)):
            self.assertIsNone(su.latest(su.REPO, '0.1.13', 'windows', Opener({url: body})), body)
        rate_limited = urllib.error.HTTPError(url, 403, 'rate limit exceeded', {}, None)
        self.assertIsNone(su.latest(su.REPO, '0.1.13', 'windows', Opener({url: rate_limited})))

    def test_check_thread(self):
        url = su.API.format(repo='a/b')
        check = su.Check('a/b', '0.1.0', 'windows', Opener({url: json.dumps(answer()).encode()}))
        check.thread.join(10)
        self.assertTrue(check.done())
        self.assertEqual(check.result.version, '9.9.9')

    def test_user_agent_names_only_the_setup(self):
        seen = {}

        def fake_urlopen(request, timeout, context):
            seen.update(request.header_items())
            return Response(b'{}')
        real = su.urllib.request.urlopen
        su.urllib.request.urlopen = fake_urlopen
        try:
            su.opener_for('0.1.13+973c626')('https://api.github.com/x', 'application/json').read()
        finally:
            su.urllib.request.urlopen = real
        self.assertEqual(seen, {'User-agent': 'XC2-Setup/0.1.13+973c626', 'Accept': 'application/json'})


class UnpackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def unpack(self, data):
        (self.dir / 'a.zip').write_bytes(data)
        out = self.dir / 'out'
        out.mkdir(exist_ok=True)
        su.unpack(self.dir / 'a.zip', out)
        return out

    def test_strips_the_one_top_folder(self):
        out = self.unpack(setup_zip())
        self.assertTrue((out / 'python' / 'pythonw.exe').is_file())
        self.assertFalse((out / 'XC2-Setup').exists())
        self.assertTrue(su.is_setup(out, '9.9.9'))
        self.assertFalse(su.is_setup(out, '9.9.10'))

    def test_keeps_files_at_the_top(self):
        out = self.unpack(setup_zip(top=''))
        self.assertTrue((out / 'VERSION').is_file())

    def test_refuses_names_outside_the_folder(self):
        for name in ('XC2-Setup/../../evil.txt', '/evil.txt', 'XC2-Setup/C:/evil.txt', 'XC2-Setup\\..\\evil.txt'):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, 'w') as z:
                z.writestr('XC2-Setup/VERSION', '9.9.9')
                z.writestr(name, 'x')
            with self.assertRaises(su.UpdateError, msg=name) as e:
                self.unpack(buf.getvalue())
            self.assertEqual(e.exception.id, 'ui.update_err_unpack')
            for where in (self.dir, self.dir.parent, Path(self.dir.anchor), self.dir / 'out' / 'C:'):
                self.assertFalse((where / 'evil.txt').exists(), (name, where))
            self.assertFalse((self.dir / 'out' / 'VERSION').exists(), 'files were written before the check')

    def test_not_a_zip(self):
        with self.assertRaises(su.UpdateError) as e:
            self.unpack(b'not a zip')
        self.assertEqual(e.exception.id, 'ui.update_err_unpack')


class InstallWindowsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.zip = setup_zip()

    def install(self, release=None, folders=None, opener=None, data=None):
        release = release or release_for(self.zip)
        opener = opener or Opener({release.url: self.zip if data is None else data})
        return run(su.Install(release, folders or [self.dir], 'windows', opener))

    def leftovers(self):
        return sorted(p.name for p in self.dir.iterdir() if p.name.endswith(('.part', '.partial')))

    def test_downloads_checks_and_unpacks(self):
        job = self.install()
        self.assertIsNone(job.error)
        self.assertEqual(job.result, self.dir / 'XC2-Setup-9.9.9')
        self.assertTrue(su.is_setup(job.result, '9.9.9'))
        self.assertFalse((job.result / su.PARTIAL).exists())
        self.assertEqual(self.leftovers(), [])
        self.assertEqual(job.phase, 'done')

    def test_a_different_file_is_refused(self):
        r = release_for(self.zip)
        for data in (self.zip[:-1] + b'!', self.zip[:-10], self.zip + b'more'):
            job = self.install(r, data=data)
            self.assertEqual(job.error.id, 'ui.update_err_damaged')
            self.assertIsNone(job.result)
            self.assertEqual(self.leftovers(), [])
            self.assertFalse((self.dir / 'XC2-Setup-9.9.9').exists())

    def test_a_zip_without_this_version_is_refused(self):
        data = setup_zip(version='9.9.8')
        job = self.install(release_for(data), data=data)
        self.assertEqual(job.error.id, 'ui.update_err_unpack')
        self.assertEqual(self.leftovers(), [])
        self.assertFalse((self.dir / 'XC2-Setup-9.9.9').exists())

    def test_download_error(self):
        r = release_for(self.zip)
        job = self.install(r, opener=Opener({r.url: urllib.error.URLError('offline')}))
        self.assertEqual(job.error.id, 'ui.update_err_download')
        self.assertIn('offline', job.error.params['detail'])
        self.assertEqual(self.leftovers(), [])

    def test_cancel(self):
        r = release_for(self.zip)
        gate = threading.Event()
        job = su.Install(r, [self.dir], 'windows', Opener({r.url: self.zip}, gate=gate))
        job.cancel()
        gate.set()
        run(job)
        self.assertIsInstance(job.error, su.Cancelled)
        self.assertEqual(self.leftovers(), [])

    def test_an_unwritable_place_gives_way_to_the_next(self):
        blocker = self.dir / 'a-file'
        blocker.write_bytes(b'')
        user = self.dir / 'user' / 'setup'
        job = self.install(folders=[blocker, user])
        self.assertEqual(job.result, user / 'XC2-Setup-9.9.9')
        job = self.install(folders=[blocker])
        self.assertEqual(job.error.id, 'ui.update_err_write')

    def test_a_setup_already_there_is_used(self):
        first = self.install().result
        r = release_for(self.zip)
        opener = Opener({})   # any request would fail with KeyError
        job = self.install(r, opener=opener)
        self.assertEqual((job.result, job.error, opener.requests), (first, None, []))

    def test_a_foreign_folder_of_that_name_is_left_alone(self):
        (self.dir / 'XC2-Setup-9.9.9').mkdir()
        (self.dir / 'XC2-Setup-9.9.9' / 'mine.txt').write_text('keep', encoding='utf-8')
        job = self.install()
        self.assertEqual(job.result, self.dir / 'XC2-Setup-9.9.9-2')
        self.assertEqual((self.dir / 'XC2-Setup-9.9.9' / 'mine.txt').read_text(encoding='utf-8'), 'keep')

    def test_partial_folders(self):
        ours = self.dir / 'XC2-Setup-9.9.9.partial'   # left by an interrupted update: removed and used again
        ours.mkdir()
        (ours / su.PARTIAL).write_bytes(b'')
        (ours / 'half.bin').write_bytes(b'x')
        self.assertIsNone(self.install().error)
        self.assertFalse(ours.exists())
        other = Path(tempfile.mkdtemp(dir=self.dir))
        theirs = other / 'XC2-Setup-9.9.9.partial'   # not marked: never touched, the next place is used
        theirs.mkdir()
        (theirs / 'theirs.txt').write_text('keep', encoding='utf-8')
        job = self.install(folders=[other, self.dir / 'user'])
        self.assertEqual(job.result, self.dir / 'user' / 'XC2-Setup-9.9.9')
        self.assertTrue((theirs / 'theirs.txt').is_file())

    def test_the_rename_waits_for_a_virus_scanner(self):
        calls, real = [], su.os.rename

        def busy_twice(src, dst):
            calls.append(src)
            if len(calls) < 3:
                raise PermissionError(13, 'in use by another process')
            real(src, dst)
        su.os.rename = busy_twice
        try:
            job = self.install()
        finally:
            su.os.rename = real
        self.assertIsNone(job.error)
        self.assertEqual(len(calls), 3)
        self.assertTrue(su.is_setup(job.result, '9.9.9'))

    def test_no_space(self):
        real = su.shutil.disk_usage
        su.shutil.disk_usage = lambda path: real(path)._replace(free=10)
        try:
            job = self.install()
        finally:
            su.shutil.disk_usage = real
        self.assertEqual(job.error.id, 'ui.update_err_space')
        self.assertEqual(self.leftovers(), [])


class InstallAppImageTest(unittest.TestCase):
    def test_placed_and_reused(self):
        data = b'\x7fELF appimage' * 1000
        r = release_for(data, 'XC2-Setup-9.9.9-linux.AppImage', 'linux')
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            job = run(su.Install(r, [folder], 'linux', Opener({r.url: data})))
            self.assertIsNone(job.error)
            self.assertEqual(job.result, folder / 'XC2-Setup-9.9.9-linux.AppImage')
            self.assertEqual(job.result.read_bytes(), data)
            if os.name != 'nt':
                self.assertTrue(os.access(job.result, os.X_OK))
            self.assertEqual([p.name for p in folder.iterdir()], [job.result.name])
            opener = Opener({})
            again = run(su.Install(r, [folder], 'linux', opener))
            self.assertEqual((again.result, opener.requests), (job.result, []))
            job.result.write_bytes(b'damaged')   # a damaged earlier copy is downloaded again
            fixed = run(su.Install(r, [folder], 'linux', Opener({r.url: data})))
            self.assertEqual(fixed.result.read_bytes(), data)


class LaunchTest(unittest.TestCase):
    def test_commands(self):
        setup = Path('C:/Games/XC2-Setup-9.9.9')
        self.assertEqual(su.launch_command(setup, 'D:/XC2', 'windows'),
                         [str(setup / 'python' / 'pythonw.exe'), '-m', 'installer.gui', '--update-install', 'D:/XC2'])
        self.assertEqual(su.launch_command(setup, '', 'windows'),
                         [str(setup / 'python' / 'pythonw.exe'), '-m', 'installer.gui'])
        image = Path('/home/deck/XC2-Setup-9.9.9-linux.AppImage')
        self.assertEqual(su.launch_command(image, '/run/media/sd/XC2', 'linux'),
                         [str(image), '--update-install', '/run/media/sd/XC2'])

    def test_the_new_setup_finds_its_own_tcl(self):
        env = su.child_env({'TCL_LIBRARY': 'old/tcl9.0', 'TK_LIBRARY': 'old/tk9.0', 'APPIMAGE': '/a', 'APPDIR': '/m',
                            'ARGV0': 'x', 'PATH': 'p', 'LOCALAPPDATA': 'l'})
        self.assertEqual(env, {'PATH': 'p', 'LOCALAPPDATA': 'l'})

    def test_places(self):
        root = Path('C:/Downloads/XC2-Setup-0.1.13')
        saved = {k: os.environ.get(k) for k in ('LOCALAPPDATA', 'APPIMAGE', 'XDG_DATA_HOME')}
        try:
            os.environ['LOCALAPPDATA'] = 'C:/Users/u/AppData/Local'
            os.environ['APPIMAGE'] = '/home/deck/Apps/XC2-Setup-0.1.13-linux.AppImage'
            os.environ['XDG_DATA_HOME'] = '/home/deck/.local/share'
            self.assertEqual(su.places(root, 'windows'),
                             [Path('C:/Downloads'), Path('C:/Users/u/AppData/Local/XC2 Recomp/setup')])
            self.assertEqual(su.places(root, 'linux'),
                             [Path('/home/deck/Apps'), Path('/home/deck/.local/share/xc2-recomp/setup')])
            del os.environ['APPIMAGE']
            self.assertEqual(su.places(root, 'linux'), [Path('/home/deck/.local/share/xc2-recomp/setup')])
            self.assertFalse(su.can_install(root, 'linux'))   # not run from an AppImage: a link to the page only
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_can_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(su.can_install(tmp, 'windows'))   # a checkout, not a release folder
            (Path(tmp) / 'python').mkdir()
            (Path(tmp) / 'python' / 'pythonw.exe').write_bytes(b'MZ')
            self.assertFalse(su.can_install(tmp, 'windows'))   # an install folder's build-kit: no XC2-Setup.exe
            (Path(tmp) / 'XC2-Setup.exe').write_bytes(b'MZ')
            self.assertTrue(su.can_install(tmp, 'windows'))
            self.assertFalse(su.can_install(tmp, 'darwin'))   # no release for it


class PreferencesTest(unittest.TestCase):
    def test_check_updates_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'launcher.json'
            self.assertTrue(preferences.load(path)['check_updates'])   # no file: on
            preferences.save({'dest': 'D:/XC2'}, path)
            self.assertTrue(preferences.load(path)['check_updates'])
            preferences.save({'dest': 'D:/XC2', 'check_updates': False}, path)
            self.assertFalse(preferences.load(path)['check_updates'])
            self.assertFalse(json.loads(path.read_text(encoding='utf-8'))['check_updates'])
            path.write_text('{"format": 1, "check_updates": "no"}', encoding='utf-8')
            self.assertTrue(preferences.load(path)['check_updates'])   # not a bool: the default


if __name__ == '__main__':
    unittest.main()
