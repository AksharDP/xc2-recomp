"""The installer window: event model, form, catalogs, cancel, a self-closing Tk run and one end-to-end run.

    .venv-py314\\Scripts\\python tests\\installer_gui_test.py

The end-to-end test runs the real CLI in --verify mode on a finished install (every step skips) and folds its events
through the same code the window uses (the fixture's step keys are recomputed for the checkout under test, because the keys fingerprint recomp/, tools/ and runtime/). The install is build/inst-gui/e2e-install: hard links to build/install-test, so
nothing there is written, except the state file and xc2.ini, which a run rewrites and which are copied. The run hashes
the 17 GB RomFS; it is skipped when the hash differs from the expected one in all three tries. On the developer machine
(RAM with a bad bit) a buffered read of a file sometimes returns flipped bits from the page cache while the file on
disk is intact (an unbuffered read of install-test's romfs.bin gives the expected hash), so such a skip is the machine,
not the installer. It is also skipped when build/install-test is absent.
"""
import io
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import layout as layouts  # noqa: E402
from installer import state as statefile  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.runner import Context  # noqa: E402
from installer.steps import all_steps  # noqa: E402
from installer.guilogic import (DONE, FAILED, PENDING, RUNNING, SKIPPED, Catalog, Form, Model, Process,  # noqa: E402
                                amount, describe_event, parse_line, parse_probe, probe_command, probe_inputs)
from installer import ini  # noqa: E402
from installer.runner import InstallError  # noqa: E402
from installer.steps import Check  # noqa: E402
from installer import steps as steps_module  # noqa: E402

INSTALLED = ROOT / 'build' / 'install-test'
FIXTURE = ROOT / 'build' / 'inst-gui' / 'e2e-install'
COPIED = ('xc2.ini', statefile.NAME)   # written by a run, so never linked
START = {'type': 'start', 'release': 'r1', 'mode': 'install', 'dest': 'D:\\T', 'log': 'D:\\T.log',
         'steps': ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']}


def fold(*events):
    m = Model()
    for e in events:
        m.apply(e)
    return m


class ParseTest(unittest.TestCase):
    def test_event_line(self):
        self.assertEqual(parse_line('{"type": "step", "step": "check", "state": "started"}\n')['step'], 'check')

    def test_noise_is_not_an_event(self):
        for line in ('', 'Traceback (most recent call last):', '{broken', '[1, 2]', '{"no_type": 1}', '{"type": 3}'):
            self.assertIsNone(parse_line(line), line)

    def test_unicode_survives(self):
        e = parse_line(json.dumps({'type': 'warning', 'id': 'x', 'params': {'p': 'Привет'}}, ensure_ascii=False))
        self.assertEqual(e['params']['p'], 'Привет')


class ModelTest(unittest.TestCase):
    def test_steps_follow_the_events(self):
        m = fold(START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current'}},
                 {'type': 'step', 'step': 'check', 'state': 'started'})
        self.assertEqual([m.states[s] for s in m.steps],
                         [RUNNING, PENDING, SKIPPED, SKIPPED, PENDING, PENDING])
        self.assertEqual(m.current, 'check')
        m.apply({'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.0})
        self.assertEqual(m.states['check'], DONE)
        self.assertIsNone(m.current)
        m.apply({'type': 'step', 'step': 'extract', 'state': 'skipped', 'reason': 'current'})
        self.assertEqual(m.states['extract'], SKIPPED)

    def test_progress_fraction(self):
        m = fold(START, {'type': 'step', 'step': 'extract', 'state': 'started'},
                 {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 5, 'total': 20, 'unit': 'bytes'})
        self.assertEqual((m.current, m.phase, m.fraction()), ('extract', 'copy', 0.25))
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'merge'})
        self.assertIsNone(m.fraction())   # a phase without size is indeterminate
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 30, 'total': 20})
        self.assertEqual(m.fraction(), 1.0)

    def test_a_new_step_resets_progress(self):
        m = fold(START, {'type': 'progress', 'step': 'check', 'phase': 'inputs', 'done': 1, 'total': 2},
                 {'type': 'step', 'step': 'extract', 'state': 'started'})
        self.assertIsNone(m.phase)
        self.assertIsNone(m.fraction())

    def test_error_marks_the_step(self):
        m = fold(START, {'type': 'step', 'step': 'check', 'state': 'started'},
                 {'type': 'error', 'id': 'check.disk_space', 'step': 'check', 'params': {'path': 'D:\\T'}},
                 {'type': 'end', 'result': 'failed', 'seconds': 2})
        self.assertEqual(m.states['check'], FAILED)
        self.assertEqual(m.error[0], 'check.disk_space')
        self.assertEqual(m.result, 'failed')

    def test_space_text(self):
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 36_400_000_000, 'free': 100_000_000_000, 'temp': 'C:\\t',
                  'temp_need': 0, 'temp_free': 5, 'same_disk': True})
        text = m.need_text(Catalog('en'))
        self.assertIn('36.4', text)
        self.assertIn('100.0', text)
        self.assertEqual(Model().need_text(Catalog('en')), '')
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 20_000_000_000, 'free': 1, 'temp': 'C:\\t',
                  'temp_need': 2_000_000_000, 'temp_free': 3, 'same_disk': False})
        self.assertIn('C:\\t', m.need_text(Catalog('en')))

    def test_warnings_collect(self):
        m = fold(START, {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}})
        self.assertEqual(m.warnings, [('warn.lang_needs_fonts', {'lang': 'ko'})])

    def test_finish_without_end_event(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'})
        m.finish(-1073741510)
        self.assertEqual(m.result, 'failed')
        self.assertEqual(m.states['lift'], FAILED)
        self.assertEqual(m.error[0], 'ui.process_died')

    def test_finish_after_cancel(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'}, {'type': 'end', 'result': 'cancelled'})
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')
        self.assertIsNone(m.error)
        self.assertEqual(m.states['lift'], PENDING)   # not finished; the next run resumes here

    def test_exit_code_3_alone_is_cancelled(self):
        m = Model()
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')

    def test_unknown_event_is_ignored(self):
        m = fold(START, {'type': 'future_thing', 'x': 1})
        self.assertEqual(m.states['check'], PENDING)


class RunStatesTest(unittest.TestCase):
    OK = [START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current',
                                                                     'prepare': 'current', 'cleanup': 'not_needed'}},
          {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
          {'type': 'step', 'step': 'extract', 'state': 'started'}, {'type': 'step', 'step': 'extract', 'state': 'done'},
          {'type': 'step', 'step': 'lift', 'state': 'skipped'}, {'type': 'step', 'step': 'compile', 'state': 'skipped'},
          {'type': 'step', 'step': 'prepare', 'state': 'skipped'}, {'type': 'step', 'step': 'cleanup', 'state': 'done'},
          {'type': 'end', 'result': 'ok', 'seconds': 1}]
    FAILED = [START, {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
              {'type': 'step', 'step': 'extract', 'state': 'started'},
              {'type': 'error', 'id': 'extract.romfs_mismatch', 'step': 'extract', 'params': {}},
              {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}},
              {'type': 'end', 'result': 'failed', 'seconds': 1}]

    def test_ok_run_has_only_done_and_skipped_steps(self):
        m = fold(*self.OK)
        m.finish(0)
        self.assertEqual(m.result, 'ok')
        self.assertIsNone(m.error)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})

    def test_a_new_run_keeps_nothing_of_the_failed_one(self):
        m = fold(*self.FAILED)
        m.finish(1)
        self.assertEqual(m.states['extract'], FAILED)
        for e in self.OK:   # the same model object fed a second run (the window makes a new one, but even then)
            m.apply(e)
        m.finish(0)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})
        self.assertIsNone(m.error)
        self.assertEqual(m.warnings, [])
        self.assertEqual(m.result, 'ok')


class FormTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for name in ('base.nsp', 'upd.nsp', 'prod.keys'):
            (self.dir / name).write_bytes(b'x')

    def tearDown(self):
        self.tmp.cleanup()

    def form(self, **kw):
        values = dict(base=str(self.dir / 'base.nsp'), update=str(self.dir / 'upd.nsp'), keys=str(self.dir / 'prod.keys'),
                      lang='en-US', dest=str(self.dir / 'out'))
        values.update(kw)
        return Form(**values)

    def test_complete_form(self):
        self.assertEqual(self.form().problems(), [])

    def test_empty_form(self):
        self.assertEqual(Form().problems(), ['ui.need_dest', 'ui.need_base', 'ui.need_update', 'ui.need_keys',
                                             'ui.need_lang'])

    def test_missing_files(self):
        self.assertEqual(self.form(base=str(self.dir / 'no.nsp'), keys=str(self.dir / 'no.keys')).problems(),
                         ['ui.missing_base', 'ui.missing_keys'])

    def test_unknown_language(self):
        self.assertEqual(self.form(lang='xx').problems(), ['ui.need_lang'])

    def test_command(self):
        cmd = self.form().command('py.exe', root=ROOT, temp=self.dir / 't', verify=True)
        self.assertEqual(cmd[:3], ['py.exe', '-m', 'installer'])
        for flag in ('--base', '--update', '--keys', '--lang', '--dest', '--root', '--temp', '--verify'):
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[cmd.index('--lang') + 1], 'en-US')

    def test_installed_folder_needs_only_itself(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0') | {'lang': 'de'})
        form = Form(dest=str(out))
        self.assertTrue(form.update_mode())
        self.assertEqual(form.problems(), [])
        cmd = form.command('py.exe')
        self.assertEqual(cmd, ['py.exe', '-m', 'installer', '--dest', str(out)])

    def test_installed_folder_still_checks_given_files(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0'))
        self.assertEqual(Form(dest=str(out), base=str(self.dir / 'no.nsp')).problems(), ['ui.missing_base'])

    def test_broken_state_is_a_new_install(self):
        out = self._mkdir(self.dir / 'out')
        (out / statefile.NAME).write_text('{nope', encoding='utf-8')
        self.assertFalse(Form(dest=str(out)).update_mode())

    def test_language_must_be_one_of_the_game_files(self):
        jp = ('ja', 'zh-CN', 'zh-TW')
        self.assertEqual(self.form(languages=jp).problems(), ['ui.lang_not_in_game'])
        self.assertEqual(self.form(lang='ja', languages=jp).problems(), [])
        self.assertEqual(self.form(lang='ru').problems(), ['ui.need_lang'])
        self.assertEqual(self.form(lang='ko').problems(), [])   # languages not read yet: the check decides

    def test_probe_inputs_and_command(self):
        base, upd, keys = (str(self.dir / n) for n in ('base.nsp', 'upd.nsp', 'prod.keys'))
        self.assertIsNone(probe_inputs('', upd, keys))
        self.assertIsNone(probe_inputs(base, upd, str(self.dir / 'no.keys')))
        self.assertEqual(probe_inputs(f' {base} ', str(self.dir / 'no.nsp'), keys), (base, None, keys))
        inputs = probe_inputs(base, upd, keys)
        self.assertEqual(inputs, (base, upd, keys))
        self.assertEqual(probe_command('py.exe', inputs),
                         ['py.exe', '-m', 'installer.probe', '--base', base, '--keys', keys, '--update', upd])
        self.assertNotIn('--update', probe_command('py.exe', (base, None, keys)))

    def test_parse_probe(self):
        event = {'type': 'languages', 'region': 'jp', 'release': 'Japan', 'languages': ['ja', 'zh-CN', 'zh-TW']}
        self.assertEqual(parse_probe('noise\n' + json.dumps(event) + '\n'), event)
        self.assertEqual(parse_probe(json.dumps({'type': 'error', 'id': 'check.keys_wrong'}))['id'], 'check.keys_wrong')
        self.assertIsNone(parse_probe('Traceback (most recent call last):\n'))

    def test_probe_reports_errors_with_check_ids(self):
        # a file that is no game: the probe answers with the check step's message id, as the install would
        cmd = [sys.executable, '-m', 'installer.probe', '--base', str(self.dir / 'base.nsp'),
               '--keys', str(self.dir / 'prod.keys')]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=120)
        event = parse_probe(r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertEqual(event['type'], 'error')
        self.assertTrue(Catalog('en').has(event['id']), event)

    @staticmethod
    def _mkdir(path):
        path.mkdir(parents=True, exist_ok=True)
        return path


class CheckLanguageTest(unittest.TestCase):
    """installer/steps.py Check.language: the language an install gets from what the game files have."""
    US = ['en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'ko']
    JP = ['ja', 'zh-CN', 'zh-TW']

    def resolve(self, requested, languages=None, state=None, recorded=None, installed=None):
        warnings = []
        with tempfile.TemporaryDirectory() as d:
            ctx = SimpleNamespace(languages=languages, state=state or {}, dest=Path(d),
                                  reporter=SimpleNamespace(warning=lambda id, **p: warnings.append(id)))
            real = steps_module.installed_languages
            steps_module.installed_languages = lambda dest: installed
            try:
                Check().language(ctx, requested, recorded)
            finally:
                steps_module.installed_languages = real
        return ctx.lang, ctx.languages, warnings

    def test_the_installed_romfs_comes_before_the_recorded_list(self):
        # a US-to-JP replacement stopped after the extraction: the RomFS is JP, the recorded list still US
        self.assertEqual(self.resolve('ja', state={'languages': self.US}, installed=self.JP)[:2], ('ja', self.JP))
        self.assertEqual(self.resolve(None, state={'languages': self.US}, installed=self.JP)[0], 'ja')

    def test_a_recorded_language_the_files_lack_gives_way(self):
        self.assertEqual(self.resolve(None, self.US, recorded='fr')[0], 'fr')
        self.assertEqual(self.resolve(None, self.JP, recorded='fr')[0], 'ja')
        self.assertEqual(self.resolve('zh-TW', self.JP, recorded='fr')[0], 'zh-TW')

    def test_default_follows_the_release(self):
        self.assertEqual(self.resolve(None, self.US)[0], 'en-US')
        self.assertEqual(self.resolve(None, self.JP)[:2], ('ja', self.JP))

    def test_a_language_the_files_lack_stops(self):
        with self.assertRaises(InstallError) as e:
            self.resolve('en-US', self.JP)
        self.assertEqual(e.exception.id, 'check.lang_not_in_game')
        self.assertEqual(e.exception.params['known'], 'ja, zh-CN, zh-TW')

    def test_recorded_languages_when_the_files_are_not_read(self):
        self.assertEqual(self.resolve('zh-TW', state={'languages': self.JP})[0], 'zh-TW')
        self.assertEqual(self.resolve(None, state={'languages': self.JP})[0], 'ja')
        with self.assertRaises(InstallError):
            self.resolve('ko', state={'languages': self.JP})

    def test_fonts_warning_and_no_languages(self):
        self.assertEqual(self.resolve('ko', self.US)[2], ['warn.lang_needs_fonts'])
        self.assertEqual(self.resolve('de')[0], 'de')   # an install from before the languages were recorded
        with self.assertRaises(InstallError) as e:
            self.resolve(None)
        self.assertEqual(e.exception.id, 'check.lang_missing')


class LanguageWindowTest(unittest.TestCase):
    """installer/gui.py: the language list follows the chosen game files (with a stand-in for the probe process) and
    the install folder's recorded language."""
    US = ['en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'ko']
    JP = ['ja', 'zh-CN', 'zh-TW']

    def setUp(self):
        try:
            import tkinter as tk
            from installer import gui
        except ImportError:
            self.skipTest('no tkinter')
        gui.fix_tcl_paths()
        try:
            self.root = tk.Tk()
        except tk.TclError:
            self.skipTest('no display')
        self.addCleanup(self.root.destroy)
        self.root.withdraw()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        for name in ('us.nsp', 'jp.nsp', 'prod.keys'):
            (self.dir / name).write_bytes(b'x')
        self.made = []
        made = self.made

        class Probe:
            def __init__(self, python, inputs, cwd=None):
                self.inputs, self.result = inputs, None
                made.append(self)

            def done(self):
                return self.result is not None

        real, gui.LanguageProbe = gui.LanguageProbe, Probe
        self.addCleanup(setattr, gui, 'LanguageProbe', real)
        self.app = gui.App(self.root, layouts.find(ROOT), remember=False)

    def choose(self, base, start=True):
        self.app.vars['base'].set(str(self.dir / base) if base else '')
        self.app.vars['keys'].set(str(self.dir / 'prod.keys'))
        if start:
            self.app.start_probe()

    def finish(self, probe, languages):
        probe.result = {'type': 'languages', 'languages': languages, 'release': 'r'}
        self.app.poll_probe()

    def test_returning_to_the_files_shown_drops_the_other_read(self):
        self.choose('us.nsp')
        a = self.made[-1]
        self.finish(a, self.US)
        self.choose('jp.nsp')
        b = self.made[-1]
        self.assertIsNot(a, b)
        self.choose('us.nsp')   # A, B, A: no new read, and B's result must not land on A
        self.assertEqual(len(self.made), 2)
        self.assertIsNone(self.app.probe)
        self.finish(b, self.JP)
        self.assertEqual(self.app.languages, self.US)
        self.assertEqual(self.app.selected_lang(), 'en-US')

    def test_a_result_for_files_no_longer_chosen_is_dropped(self):
        self.choose('us.nsp')
        self.finish(self.made[-1], self.US)
        self.choose('jp.nsp')
        b = self.made[-1]
        self.choose('us.nsp', start=False)   # the field changed; its own read waits for the debounce
        self.finish(b, self.JP)
        self.assertEqual(self.app.languages, self.US)

    def test_after_a_failed_read_another_folder_reads_again(self):
        jp, us = self.dir / 'jp-install', self.dir / 'us-install'
        for folder, lang, langs in ((jp, 'ja', self.JP), (us, 'fr', self.US)):
            folder.mkdir()
            statefile.save(folder, statefile.empty('r0') | {'lang': lang, 'languages': langs})
        self.app.vars['dest'].set(str(jp))
        self.choose('us.nsp')
        self.made[-1].result = {'type': 'error', 'id': 'check.languages_unreadable'}
        self.app.poll_probe()
        self.assertEqual(self.app.languages, self.JP)   # the folder's list stands in for the unreadable files
        self.app.vars['dest'].set(str(us))
        self.app.start_probe()
        self.assertEqual(len(self.made), 2)
        self.made[-1].result = {'type': 'error', 'id': 'check.languages_unreadable'}
        self.app.poll_probe()
        self.assertEqual(self.app.languages, self.US)
        self.assertEqual(self.app.selected_lang(), 'fr')

    def test_a_failed_read_is_translated_after_a_language_change(self):
        self.choose('us.nsp')
        self.made[-1].result = {'type': 'error', 'id': 'check.languages_unreadable'}
        self.app.poll_probe()
        self.assertIn(Catalog('en')('check.languages_unreadable'), self.app.langs_label.cget('text'))
        self.app.ui_var.set('zh-Hans')   # the reason is kept as its check id, so the line changes with the language
        self.app.switch_language()
        self.assertIn(Catalog('zh-Hans')('check.languages_unreadable'), self.app.langs_label.cget('text'))

    def test_the_install_folders_language_is_kept(self):
        jp, us = self.dir / 'jp-install', self.dir / 'us-install'
        for folder, lang, langs in ((jp, 'ja', self.JP), (us, 'fr', self.US)):
            folder.mkdir()
            statefile.save(folder, statefile.empty('r0') | {'lang': lang, 'languages': langs})
        self.choose(None)
        self.app.vars['dest'].set(str(jp))
        self.app.start_probe()
        self.assertEqual(self.app.selected_lang(), 'ja')
        self.app.vars['dest'].set(str(us))   # fr is checked against the JP list first, then the list becomes US
        self.app.start_probe()
        self.assertEqual(self.app.selected_lang(), 'fr')


class CatalogTest(unittest.TestCase):
    def test_catalog_completeness_and_placeholders(self):
        catalogs = sorted(p.stem for p in (ROOT / 'installer' / 'text').glob('*.json'))
        self.assertEqual(catalogs, ['en', 'zh-Hans', 'zh-Hant'])
        self.assertEqual(Catalog('ru').lang, 'en')   # a language without a catalog falls back to English
        en = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        for lang in ('zh-Hans', 'zh-Hant'):
            other = json.loads((ROOT / 'installer' / 'text' / f'{lang}.json').read_text(encoding='utf-8'))
            self.assertEqual(set(other), set(en), f'{lang}.json keys differ from en.json')
            for k, v in en.items():
                self.assertEqual(set(re.findall(r'\{(\w+)\}', other[k])), set(re.findall(r'\{(\w+)\}', v)),
                                 f'{lang}.json placeholder mismatch for {k}')

    def test_every_id_the_window_uses_exists(self):
        ids = ['ui.' + n for n in ('title', 'start', 'cancel', 'open_folder', 'notice', 'space_one', 'space_two',
                                   'process_died', 'result_ok', 'result_failed', 'result_cancelled', 'langs_hint',
                                   'langs_checking', 'langs_found', 'langs_failed', 'lang_not_in_game')]
        ids += [f'step.{s}' for s in ('check', 'extract', 'lift', 'compile', 'prepare', 'cleanup')]
        ids += ['ui.state_' + n for n in ('started', 'done', 'skipped', 'pending', 'running', 'failed')]
        ids += ['lang.' + c for c in ini.LANGUAGES]
        for lang in ('en', 'zh-Hans', 'zh-Hant'):
            cat = Catalog(lang)
            self.assertEqual([i for i in ids if not cat.has(i)], [], f'missing ids in {lang}')

    def test_codes_become_names(self):
        cat = Catalog('en')
        self.assertIn('Korean', cat('warn.lang_needs_fonts', lang='ko'))
        self.assertNotIn(' ko ', cat('warn.lang_needs_fonts', lang='ko'))
        self.assertNotIn('extract', cat('state.output_changed', step='extract'))
        for word in ('base', 'update', 'keys'):
            self.assertIn(cat('param.input.' + word), cat('check.keys_wrong', input=word))
        self.assertEqual(cat('check.lang_unknown', lang='xx', known='ja, en-US'),
                         "Unknown language 'xx'. Known: Japanese, English (US).")
        self.assertEqual(cat('check.lang_not_in_game', lang='en-US', known='ja, zh-CN, zh-TW'),
                         'These game files have no English (US) text. Their languages: Japanese, '
                         'Chinese (Simplified, China), Chinese (Traditional, Taiwan).')
        zh = Catalog('zh-Hans')
        self.assertIn('日版', zh('ui.langs_found', release='Japan', known='ja, zh-CN'))
        self.assertIn('简体中文（中国大陆）', zh('ui.langs_found', release='Japan', known='ja, zh-CN'))

    def test_fallback_and_params(self):
        cat = Catalog('en')
        self.assertIn('36.4', cat('ui.log_space', need='36.4', free='1.0'))
        self.assertEqual(cat('no.such.id'), 'no.such.id')
        self.assertIn('a, b', cat('check.release_incomplete', paths=['a', 'b']))

    def test_describe_and_amount(self):
        cat = Catalog('en')
        self.assertEqual(amount(cat, 6e9, 16e9, 'bytes'), '6.0 / 16.0 GB')
        self.assertEqual(amount(cat, 1, 9, 'shaders'), '1 / 9 shaders')
        self.assertEqual(amount(cat, 1, 9, 'widgets'), '1 / 9 widgets')
        line = describe_event(cat, {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.5})
        self.assertIn('1.5 s', line)
        self.assertEqual(describe_event(cat, {'type': 'start'}), '')


BREAK_CHILD = 'import time\nprint("ready", flush=True)\ntime.sleep(60)\n'


class CancelTest(unittest.TestCase):
    def test_ctrl_break_ends_the_process_group(self):
        p = Process([sys.executable, '-c', BREAK_CHILD])
        try:
            self.assertEqual(p.lines.get(timeout=20).strip(), 'ready')
            t0 = time.monotonic()
            p.cancel()
            code = None
            while code is None and time.monotonic() - t0 < 20:
                code = p.poll()
                time.sleep(0.05)
            self.assertIsNotNone(code, 'the child ignored Ctrl+Break')
            self.assertNotEqual(code, 0)
        finally:
            p.kill()


class SmokeTest(unittest.TestCase):
    def test_every_screen_in_both_languages(self):
        try:
            import tkinter  # noqa: F401
        except ImportError:
            self.skipTest('no tkinter')
        r = subprocess.run([sys.executable, '-m', 'installer.gui', '--selftest'], cwd=ROOT, timeout=120,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])


def make_fixture():
    """build/inst-gui/e2e-install: links to install-test (see the module docstring), with the step keys of the state
    set to the ones this checkout computes, so the install counts as current for it."""
    if not (FIXTURE / statefile.NAME).is_file():
        shutil.rmtree(FIXTURE, ignore_errors=True)
        for folder, _dirs, files in os.walk(INSTALLED):
            target = FIXTURE / Path(folder).relative_to(INSTALLED)
            target.mkdir(parents=True, exist_ok=True)
            for name in files:
                src, dst = Path(folder) / name, target / name
                if name in COPIED and target == FIXTURE:
                    shutil.copy2(src, dst)
                else:
                    os.link(src, dst)
    layout = layouts.find(ROOT)
    state = statefile.load(FIXTURE)
    options = SimpleNamespace(base=None, update=None, keys=None, lang=None)
    ctx = Context(layout, FIXTURE, ROOT / 'build' / 'inst-gui' / 'e2e-temp', Reporter(out=io.StringIO()), options, state=state)
    ctx.steps = all_steps()
    ctx.plan()
    for step_id, record in state['steps'].items():
        record['key'] = ctx.keys[step_id]
    statefile.save(FIXTURE, state)
    return FIXTURE


@unittest.skipUnless((INSTALLED / statefile.NAME).is_file(), 'build/install-test is absent')
class EndToEndTest(unittest.TestCase):
    @staticmethod
    def run_cli(form, temp, tries=1):
        """Runs the CLI in --verify mode through Process, up to `tries` times while the RomFS hash reads wrong."""
        for attempt in range(tries):
            p = Process(form.command(sys.executable, root=ROOT, temp=temp, verify=True), cwd=ROOT)
            model, stray = Model(), []
            t0, code = time.monotonic(), None
            while code is None and time.monotonic() - t0 < 600:
                try:
                    line = p.lines.get(timeout=0.2)
                except queue.Empty:
                    line = ''
                if line:
                    event = parse_line(line)
                    if event:
                        model.apply(event)
                    elif line.strip():
                        stray.append(line)
                code = p.poll()
            p.kill()
            model.finish(code)
            if not any(w[0] == 'state.output_changed' for w in model.warnings):
                break
        return code, model, stray, time.monotonic() - t0

    def test_verify_run_skips_every_step(self):
        form = Form(dest=str(make_fixture()))
        self.assertTrue(form.update_mode())
        code, model, stray, elapsed = self.run_cli(form, ROOT / 'build' / 'inst-gui' / 'e2e-temp', tries=3)
        print(f'\nend-to-end: exit {code}, result {model.result}, {elapsed:.1f} s, states {model.states}')
        if any(w[0] == 'state.output_changed' for w in model.warnings):
            self.skipTest('the RomFS hash read wrong three times (page cache of the faulty RAM, see the module docstring)')
        self.assertEqual(code, 0, stray[-5:] or model.error)
        self.assertEqual(model.result, 'ok')
        self.assertIn(model.mode, ('resume', 'update'))
        self.assertEqual({s: model.states[s] for s in model.steps if s not in ('check', 'cleanup')},
                         {s: SKIPPED for s in model.steps if s not in ('check', 'cleanup')})
        self.assertIsNotNone(model.space)
        self.assertEqual(stray, [])

    def test_cancel_during_the_check(self):
        cmd = Form(dest=str(make_fixture())).command(sys.executable, root=ROOT, temp=ROOT / 'build' / 'inst-gui' / 'e2e-temp',
                                                     verify=True)
        p = Process(cmd, cwd=ROOT)
        model, t0, code, cancelled = Model(), time.monotonic(), None, False
        while code is None and time.monotonic() - t0 < 120:
            try:
                event = parse_line(p.lines.get(timeout=0.2))
            except queue.Empty:
                event = None
            if event:
                model.apply(event)
                if event.get('phase') == 'verify_romfs' and not cancelled:
                    cancelled = True
                    p.cancel()
            code = p.poll()
        p.kill()
        model.finish(code)
        self.assertTrue(cancelled)
        self.assertEqual((code, model.result), (3, 'cancelled'))
        self.assertIsNone(model.error)


if __name__ == '__main__':
    unittest.main(verbosity=2)
