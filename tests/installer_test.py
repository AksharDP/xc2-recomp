"""installer/ bookkeeping with fake steps: fresh install, resume after a failure or cancel, transient steps, update
with a newer release, xc2.ini and the event stream. No game files, no tools; stdlib only.

Run: python tests/installer_test.py
"""
import io
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import ini, runner, state, steps  # noqa: E402
from installer import layout as layouts  # noqa: E402
from installer.events import LauncherReporter, Reporter  # noqa: E402
from installer.layout import Layout  # noqa: E402


class Fake(runner.Step):
    """A step whose output is <dir>/<id>.out holding its key; dir is the temp folder for a transient step."""

    def __init__(self, id, transient=False, always=False, previous=None):
        self.id, self.transient, self.always, self.previous = id, transient, always, previous

    def out(self, ctx):
        return (ctx.temp if self.transient else ctx.dest) / f'{self.id}.out'

    def key(self, ctx):
        upstream = ctx.keys[self.previous] if self.previous else ''
        return f'{self.id}:{ctx.options.inputs.get(self.id, "v1")}:{upstream}'

    def current(self, ctx, record):
        p = self.out(ctx)
        return p.is_file() and p.read_text() == ctx.keys[self.id]

    def verify(self, ctx, record):
        ctx.options.verified.append(self.id)
        return ctx.options.verify_ok.get(self.id, True)

    def run(self, ctx):
        o = ctx.options
        o.ran.append(self.id)
        if self.id in ctx.forced:
            o.forced.append(self.id)
        if self.id == 'check':
            ctx.verify_kept(self.id)
        failure = o.fail.pop(self.id, None)
        if failure == 'error':
            raise runner.InstallError('fake.failed', step=self.id)
        if failure == 'cancel':
            raise KeyboardInterrupt
        if self.id == 'cleanup':
            for p in ctx.temp.glob('*.out'):
                p.unlink()
            return {}
        p = self.out(ctx)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(ctx.keys[self.id])
        return {'note': self.id}


def fake_steps():
    return [Fake('check', always=True), Fake('extract'), Fake('lift', transient=True, previous='extract'),
            Fake('compile', previous='lift'), Fake('prepare', previous='compile'), Fake('cleanup', always=True)]


class Bed:
    """A dest and temp folder plus one run of the fake steps per call."""

    def __init__(self, base):
        self.dest, self.temp = base / 'dest', base / 'temp'
        self.temp.mkdir()

    def run(self, release='1.0', inputs=None, fail=None, verify_ok=None, verify=False):
        layout = Layout(root=Path('.'), dev=True, release=release, python=Path('python'), tools=Path('tools'),
                        recomp=Path('recomp'), llvm=Path('llvm'),
                        deps=Path('deps'), runtime=Path('rt'), runtime_lib=Path('lib'), ninja=Path('ninja'),
                        xlate=Path('xlate'))
        options = SimpleNamespace(inputs=inputs or {}, fail=dict(fail or {}), verify_ok=verify_ok or {}, ran=[],
                                  verified=[], forced=[])
        out = io.StringIO()
        ctx = runner.Context(layout, self.dest, self.temp, Reporter(out=out), options, state=state.load(self.dest),
                             verify=verify)
        mode = ctx.mode
        result = runner.run_steps(fake_steps(), ctx)
        events = [json.loads(line) for line in out.getvalue().splitlines()]
        return SimpleNamespace(result=result, ran=options.ran, verified=options.verified, forced=options.forced,
                               events=events, mode=mode,
                               state=state.load(self.dest))

    @staticmethod
    def skipped(r):
        return {e['step']: e['reason'] for e in r.events if e['type'] == 'step' and e['state'] == 'skipped'}


ALL = ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bed = Bed(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_fresh_install_runs_everything_and_records_it(self):
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'install', ALL))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift', 'compile', 'prepare'})
        self.assertEqual(r.state['steps']['compile']['note'], 'compile')
        self.assertEqual(r.state['release'], '1.0')
        started = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'started']
        done = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'done']
        self.assertEqual((started, done), (ALL, ALL))

    def test_rerun_skips_finished_steps_and_unneeded_transient(self):
        self.bed.run()
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'cleanup']))
        self.assertEqual(self.bed.skipped(r), {'extract': 'current', 'lift': 'not_needed', 'compile': 'current',
                                               'prepare': 'current'})
        self.assertEqual(r.verified, [])   # same release: no full checks

    def test_resume_after_failure(self):
        r = self.bed.run(fail={'compile': 'error'})
        self.assertEqual((r.result, r.ran), ('failed', ['check', 'extract', 'lift', 'compile']))
        error = [e for e in r.events if e['type'] == 'error'][0]
        self.assertEqual((error['id'], error['step'], error['params']), ('fake.failed', 'compile', {'step': 'compile'}))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift'})
        r = self.bed.run()
        # lift's output is still in the temp folder (no cleanup ran), so only compile and later run.
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'compile', 'prepare', 'cleanup']))

    def test_resume_after_cancel_with_transient_output_gone(self):
        r = self.bed.run(fail={'prepare': 'cancel'})
        self.assertEqual(r.result, 'cancelled')
        self.assertNotIn('prepare', r.state['steps'])
        for p in self.bed.temp.glob('*.out'):
            p.unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'prepare', 'cleanup'])
        self.assertEqual(self.bed.skipped(r)['lift'], 'not_needed')

    def test_failed_check_writes_nothing(self):
        r = self.bed.run(fail={'check': 'error'})
        self.assertEqual(r.result, 'failed')
        self.assertFalse(self.bed.dest.exists())

    def test_removed_output_runs_again(self):
        self.bed.run()
        (self.bed.dest / 'compile.out').unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'cleanup'])

    def test_update_with_new_lifter_keeps_extraction(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', inputs={'lift': 'v2'})
        self.assertEqual((r.result, r.mode), ('ok', 'update'))
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'prepare', 'cleanup'])
        self.assertEqual(r.verified, ['extract'])   # kept from another release: full check
        self.assertEqual(r.state['steps']['extract']['release'], '1.0')
        self.assertEqual(r.state['steps']['compile']['release'], '1.1')
        self.assertEqual(r.state['release'], '1.1')

    def test_update_that_changes_nothing_skips_the_rebuild(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1')
        self.assertEqual(r.ran, ['check', 'cleanup'])
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_update_with_changed_kept_files_extracts_again(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', verify_ok={'extract': False})
        self.assertEqual(r.ran, ['check', 'extract', 'cleanup'])
        self.assertEqual(r.forced, ['extract'])   # the step knows not to trust what it left behind
        warnings = [e for e in r.events if e['type'] == 'warning']
        self.assertEqual([(w['id'], w['params']) for w in warnings], [('state.output_changed', {'step': 'extract'})])

    def test_verify_option_checks_same_release(self):
        self.bed.run()
        r = self.bed.run(verify=True)
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_state_errors(self):
        self.bed.dest.mkdir()
        (self.bed.dest / state.NAME).write_text('{not json')
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.unreadable')
        (self.bed.dest / state.NAME).write_text(json.dumps({'format': 99, 'steps': {}}))
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.newer_format')


class IniTest(unittest.TestCase):
    EXAMPLE = '[display]\n# vk_present = mailbox\n\n[language]\n# locale = en-US\n# lang = en-US\n# x = 1\n'

    def test_example_line_is_activated(self):
        out = ini.set_language(self.EXAMPLE, 'fr')
        self.assertIn('\nlang = fr\n', out)
        self.assertNotIn('# lang', out)
        self.assertIn('# locale = en-US', out)

    def test_active_line_is_rewritten_and_the_rest_kept(self):
        text = '[display]\nvk_present = fifo\n[language]\n  LANG =  ja  \nfps60 = 0\n'
        out = ini.set_language(text, 'de')
        self.assertEqual(out, '[display]\nvk_present = fifo\n[language]\n  lang =  de\nfps60 = 0\n')
        self.assertEqual(ini.get_language(out), 'de')

    def test_later_copies_of_the_key_are_removed(self):
        # the runtime takes the last value, so a second active line would undo the one written
        text = '[language]\ngame_langs = en-US ko\nlang = fr\nxcx_game_langs = de\n# game_langs = it\n'
        out = ini.set_value(text, 'game_langs', 'ja zh-CN zh-TW')
        self.assertEqual(out, '[language]\ngame_langs = ja zh-CN zh-TW\nlang = fr\n# game_langs = it\n')
        self.assertEqual(ini.get_value(out, 'game_langs'), 'ja zh-CN zh-TW')

    def test_languages_without_text_are_not_offered(self):
        # Neither release has Dutch, Portuguese or Russian text; asking for one gave a black screen (GitHub issue 107).
        # The game knows no zh-Hans or zh-Hant: Chinese is zh-CN and zh-TW.
        for code in ('nl', 'pt', 'pt-BR', 'ru', 'zh-Hans', 'zh-Hant'):
            self.assertNotIn(code, ini.LANGUAGES)
        self.assertFalse(set(ini.DROPPED_LANGUAGES) & set(ini.LANGUAGES))
        self.assertTrue(set(ini.REPLACED.values()) <= set(ini.LANGUAGES))

    def test_recorded_and_default_language(self):
        self.assertIsNone(ini.recorded_language('ru'))
        self.assertIsNone(ini.recorded_language(None))
        self.assertEqual(ini.recorded_language('zh-Hans'), 'zh-CN')
        self.assertEqual(ini.recorded_language('zh-Hant'), 'zh-TW')
        self.assertEqual(ini.recorded_language('de'), 'de')
        self.assertEqual(ini.default_language(['en-GB', 'en-US', 'ko']), 'en-US')
        self.assertEqual(ini.default_language(['ja', 'zh-CN', 'zh-TW']), 'ja')

    def test_codes_match_the_runtime(self):
        # runtime/cfg.c's XC2_LANG values and runtime/ov_system.c game_codes are the installer's codes
        cfg = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
        values = re.search(r'\{"LANG", "", "([^"]+)"', cfg).group(1).split('|')
        self.assertEqual(sorted(values), sorted(ini.LANGUAGES))
        ov = (ROOT / 'runtime' / 'ov_system.c').read_text(encoding='utf-8')
        codes = re.findall(r'"([^"]+)"', re.search(r'game_codes\[\] = \{([^}]+)\}', ov).group(1))
        self.assertEqual(sorted(codes), sorted(ini.LANGUAGES))

    def test_game_langs_is_written_beside_lang(self):
        example = self.EXAMPLE + '# game_langs = en-US\n'
        out = ini.set_value(ini.set_language(example, 'ja'), 'game_langs', 'ja zh-CN zh-TW')
        self.assertIn('\nlang = ja\n', out)
        self.assertIn('\ngame_langs = ja zh-CN zh-TW\n', out)
        self.assertEqual(ini.get_value(out, 'game_langs'), 'ja zh-CN zh-TW')
        self.assertEqual(ini.get_language(out), 'ja')   # `lang` does not match the game_langs line
        again = ini.set_value(out, 'game_langs', 'en-US ko')
        self.assertEqual(again.count('game_langs'), 1)
        with tempfile.TemporaryDirectory() as d:
            path = ini.write(d, 'en-US', game_langs=['en-US', 'ko'])
            self.assertEqual(ini.get_value(path.read_text(), 'game_langs'), 'en-US ko')

    def test_missing_section_is_added(self):
        self.assertEqual(ini.set_language('fps60 = 0\n', 'fr'), 'fps60 = 0\n\n[language]\nlang = fr\n')
        self.assertEqual(ini.set_language('[language]\nlocale = de-DE\n', 'fr'),
                         '[language]\nlang = fr\nlocale = de-DE\n')

    def test_write_keeps_user_lines(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            example = d / 'xc2.ini.example'
            example.write_text(self.EXAMPLE)
            path = ini.write(d / '', 'en-GB', example)
            self.assertEqual(ini.get_language(path.read_text()), 'en-GB')
            path.write_text(path.read_text().replace('# vk_present = mailbox', 'vk_present = fifo'))
            ini.write(d, 'ja', example)
            text = path.read_text()
            self.assertIn('vk_present = fifo', text)
            self.assertEqual(ini.get_language(text), 'ja')


def fake_release(root, version='1.0+abc'):
    """A folder release_layout reads as a release: VERSION, the Python file and one file per other kit item."""
    for name in layouts.KIT_ITEMS:
        if '.' in name or name == 'VERSION':
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(version + '\n' if name == 'VERSION' else name)
        else:
            (root / name).mkdir(parents=True, exist_ok=True)
            (root / name / 'f.txt').write_text(name)
    layouts.release_python(root).parent.mkdir(parents=True, exist_ok=True)
    layouts.release_python(root).write_bytes(b'')
    (root / 'XC2-Setup.exe').write_bytes(b'launcher')
    (root / 'installer' / '__pycache__').mkdir()
    (root / 'installer' / '__pycache__' / 'x.pyc').write_bytes(b'')
    return layouts.release_layout(root)


# KitTest and CleanupTest adapted from totk-pc-recomp tests/installer_test.py @ e7c901b and 35c590b (MIT,
# Copyright (c) 2026 Valiantsin Dzerakh).
@unittest.skipUnless(steps.os.name == 'nt', 'the build kit is Windows-only')
class KitTest(unittest.TestCase):
    """The build kit: the setup's release folder copied into the install folder, the later steps and the mod
    command pointed at it, and an unfinished setup's temp folder taken over."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.setup, self.dest = base / 'XC2-Setup', base / 'game'
        self.dest.mkdir()
        self.layout = fake_release(self.setup)
        self.missing = mock.patch.object(Layout, 'missing', lambda self: [])
        self.missing.start()

    def tearDown(self):
        self.missing.stop()
        self.tmp.cleanup()

    def ctx(self, layout):
        return SimpleNamespace(layout=layout, dest=self.dest, reporter=Reporter(out=io.StringIO()))

    def install(self):
        ctx = self.ctx(self.layout)
        kit = steps.Kit()
        self.assertFalse(kit.current(ctx, {}))
        kit.run(ctx)
        kit.settle(ctx)
        return ctx, kit

    def test_copies_the_release_and_the_next_steps_run_from_it(self):
        ctx, kit = self.install()
        folder = self.dest / 'build-kit'
        self.assertEqual(ctx.layout.root, folder.resolve())
        self.assertEqual(ctx.layout.release, '1.0+abc')
        self.assertEqual(sorted(p.name for p in folder.iterdir()), sorted(layouts.KIT_ITEMS))
        self.assertFalse((folder / 'installer' / '__pycache__').exists())
        self.assertFalse((self.dest / 'build-kit.new').exists() or (self.dest / 'build-kit.old').exists())
        self.assertTrue(kit.current(self.ctx(self.layout), {}))

    def test_a_setup_run_from_the_kit_keeps_it(self):
        ctx, kit = self.install()
        (self.dest / 'build-kit' / 'tools' / 'mark').write_text('kept')
        self.assertTrue(kit.current(ctx, {}))
        kit.run(ctx)
        kit.settle(ctx)
        self.assertEqual((self.dest / 'build-kit' / 'tools' / 'mark').read_text(), 'kept')

    def test_a_newer_setup_replaces_the_kit(self):
        self.install()
        (self.dest / 'build-kit' / 'tools' / 'old-only').write_text('old')
        self.layout = fake_release(Path(self.tmp.name) / 'XC2-Setup-2', '1.1+def')
        ctx, _ = self.install()
        self.assertEqual(ctx.layout.release, '1.1+def')
        self.assertFalse((self.dest / 'build-kit' / 'tools' / 'old-only').exists())

    def test_mod_command_is_relative_to_the_install_folder(self):
        ctx, _ = self.install()
        cmd = steps.mod_command(ctx)
        kit = Path('build-kit')
        self.assertEqual(cmd[:2], [str(kit / 'python' / 'python.exe'), str(kit / 'tools' / 'mod_code.py')])
        self.assertEqual(cmd[cmd.index('--clang') + 1], str(kit / 'llvm' / 'bin' / 'clang.exe'))
        self.assertEqual(cmd[cmd.index('--runtime') + 1], str(kit / 'runtime'))
        self.assertEqual(cmd[cmd.index('--cache') + 1], 'build-cache')
        self.assertFalse(any(str(self.dest) in a or str(self.setup) in a for a in cmd))

    def test_dev_mode_has_no_kit(self):
        ctx = self.ctx(SimpleNamespace(dev=True, release='x', root=ROOT))
        kit = steps.Kit()
        self.assertTrue(kit.current(ctx, {}))
        self.assertEqual(kit.run(ctx), {})
        kit.settle(ctx)
        self.assertFalse((self.dest / 'build-kit').exists())

    def test_temp_folder_of_an_unfinished_setup_moves_in(self):
        old = layouts.temp_folder(self.setup / 'temp', self.dest)
        (old / 'gen').mkdir(parents=True)
        (old / 'gen' / '.install-key').write_text('k')
        temp = layouts.temp_for(self.layout, self.dest)
        self.assertEqual(temp, self.dest / 'build-cache' / 'work')
        self.assertIn('moved', steps.adopt_old_temp(self.layout, self.dest, temp))
        self.assertEqual((temp / 'gen' / '.install-key').read_text(), 'k')
        self.assertFalse(old.exists())
        self.assertIsNone(steps.adopt_old_temp(self.layout, self.dest, temp))   # once: the new one exists now

    def test_no_temp_folder_to_take_over(self):
        temp = layouts.temp_for(self.layout, self.dest)
        self.assertIsNone(steps.adopt_old_temp(self.layout, self.dest, temp))
        self.assertFalse(temp.exists())


class CleanupTest(unittest.TestCase):
    """The cleanup empties the temp folder and writes the command the game builds its mods' code with."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp, root = base / 'game', base / 'temp', base / 'release'
        self.dest.mkdir()
        temp.mkdir()
        self.root = root
        layout = SimpleNamespace(ini_example=ROOT / 'xc2.ini.example', dev=False, root=root, tools=root / 'tools',
                                 python=root / 'python' / 'python.exe', runtime=root / 'runtime',
                                 clang=root / 'llvm' / 'bin' / 'clang.exe', sysroot=None)
        self.ctx = SimpleNamespace(reporter=Reporter(out=io.StringIO()), dest=self.dest, temp=temp, lang='en-US',
                                   languages=None, state={}, layout=layout)

    def tearDown(self):
        self.tmp.cleanup()

    def lines(self):
        steps.Cleanup().run(self.ctx)
        return (self.dest / 'build-cache' / 'modcode.txt').read_text(encoding='utf-8').splitlines()

    @mock.patch.dict(os.environ, {'APPIMAGE': ''})
    def test_empties_temp_and_writes_the_mod_command(self):
        for name in ('gen', 'build', 'check', 'logs'):
            (self.ctx.temp / name).mkdir()
        lines = self.lines()
        self.assertEqual([p.name for p in self.ctx.temp.iterdir()], ['logs'])
        self.assertEqual(lines, [str(self.root / 'python' / 'python.exe'), str(self.root / 'tools' / 'mod_code.py'),
                                 '--cache', 'build-cache', '--runtime', str(self.root / 'runtime'), '--clang',
                                 str(self.root / 'llvm' / 'bin' / 'clang.exe')])

    def test_appimage_mod_command_starts_the_appimage(self):
        # The AppImage's mount, where its Python lives, is gone once the setup exits
        appimage = Path(self.tmp.name) / 'XC2-Setup-x86_64.AppImage'
        appimage.write_bytes(b'')
        with mock.patch.dict('os.environ', {'APPIMAGE': str(appimage)}):
            self.assertEqual(self.lines(), [str(appimage.resolve()), '--mod-code', '--cache', 'build-cache'])

    def test_appimage_variable_without_the_file_keeps_the_python_command(self):
        with mock.patch.dict('os.environ', {'APPIMAGE': str(Path(self.tmp.name) / 'gone.AppImage')}):
            self.assertEqual(self.lines()[1], str(self.root / 'tools' / 'mod_code.py'))


class LiftTest(unittest.TestCase):
    """Lift.run with the generator replaced: the mods stay out of it, and gen/ of an unfinished run stays."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, self.temp = base / 'game', base / 'temp'
        (self.dest / 'mods' / 'A' / 'exefs').mkdir(parents=True)
        (self.dest / 'mods' / 'A' / 'exefs' / 'x.pchtxt').write_text('1')
        (self.temp / 'gen').mkdir(parents=True)
        (self.temp / 'gen' / 'kept').write_text('x')
        self.calls, self.code = [], 0
        rep = SimpleNamespace(progress=lambda *a, **k: None, log=lambda *a: None)
        layout = SimpleNamespace(recomp=ROOT / 'recomp', tools=ROOT / 'tools')
        self.ctx = SimpleNamespace(dest=self.dest, temp=self.temp, layout=layout, reporter=rep,
                                   keys={'lift': 'k', 'extract': 'e'}, state={}, region=None)
        self.patcher = mock.patch.object(steps, 'run_tool', self.run_tool)
        self.patcher.start()

    def tearDown(self):
        self.patcher.stop()
        self.tmp.cleanup()

    def run_tool(self, ctx, step, script, args, parse=None):
        self.calls.append(script.name)
        return self.code, 'log'

    def test_lifts_without_the_mods(self):
        steps.Lift().run(self.ctx)
        self.assertEqual(self.calls, ['gen.py'])
        self.assertTrue((self.temp / 'gen' / 'kept').is_file())
        self.assertEqual((self.temp / 'gen' / steps.STAMP).read_text().strip(), 'k')

    def test_the_key_does_not_follow_the_mods(self):
        before = steps.Lift().key(self.ctx)
        (self.dest / 'mods' / 'A' / 'exefs' / 'x.pchtxt').write_text('2')
        self.assertEqual(before, steps.Lift().key(self.ctx))

    def test_a_failed_lift_leaves_no_stamp(self):
        self.code = 1
        (self.temp / 'gen' / steps.STAMP).write_text('old')
        with self.assertRaises(steps.InstallError):
            steps.Lift().run(self.ctx)
        self.assertFalse((self.temp / 'gen' / steps.STAMP).exists())


class EventsTest(unittest.TestCase):
    def test_lines_are_json_and_the_log_is_english(self):
        with tempfile.TemporaryDirectory() as d:
            out = io.StringIO()
            rep = Reporter(out=out, log_path=Path(d) / 'x.log')
            rep.step('check', 'started')
            rep.progress('extract', 'copy', 0, 10, 'bytes')
            rep.progress('extract', 'copy', 5, 10, 'bytes')    # throttled
            rep.progress('extract', 'copy', 10, 10, 'bytes')   # last one always goes out
            rep.error('check.disk_space', {'path': 'D:\\x', 'need_gb': 40, 'free_gb': 3}, step='check')
            rep.close()
            events = [json.loads(line) for line in out.getvalue().splitlines()]
            self.assertEqual([e['type'] for e in events], ['step', 'progress', 'progress', 'error'])
            self.assertEqual([e.get('done') for e in events[1:3]], [0, 10])
            self.assertTrue(all('t' in e for e in events))
            log = (Path(d) / 'x.log').read_text(encoding='utf-8')
            self.assertIn('40 GB needed, 3 GB free', log)
            self.assertIn('[check.disk_space]', log)

    def test_launcher_lines_count_steps_and_phases(self):
        out = io.StringIO()
        rep = LauncherReporter(out=out, err=io.StringIO())
        rep.emit('start', steps=['check', 'extract'])
        rep.step('extract', 'started')
        rep.progress('extract', 'copy', 5, 10, 'bytes')
        self.assertEqual(out.getvalue().splitlines(),
                         ['progress 0.500 Extracting the game', 'progress 0.750 Copying the game data'])

    def test_launcher_error_goes_to_stderr_as_text(self):
        out, err = io.StringIO(), io.StringIO()
        rep = LauncherReporter(out=out, err=err)
        rep.error('check.disk_space', {'path': 'D:\\x', 'need_gb': 40, 'free_gb': 3}, step='check')
        self.assertEqual(out.getvalue(), '')
        self.assertIn('40 GB needed, 3 GB free', err.getvalue())

    def test_catalog_has_every_id_the_code_uses(self):
        catalog = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        used = set()
        for path in (ROOT / 'installer').glob('*.py'):
            source = path.read_text(encoding='utf-8')   # gui.py holds non-ASCII text; the code page cannot decode it
            used |= set(re.findall(r"InstallError\('([a-z_.]+)'", source))
            used |= set(re.findall(r"warning\('([a-z_.]+)'", source))
            used |= {f'phase.{s}.{p}' for s, p in re.findall(r"progress\('(\w+)', '(\w+)'[,)]", source)}
        self.assertEqual(sorted(i for i in used if i not in catalog), [])


if __name__ == '__main__':
    unittest.main()
