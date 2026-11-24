"""tools/mod_code.py: which functions go into a mod library, the switch numbers of its patch sets, what a refused set costs,
and that an interrupted run is run again. No game data: the tables are made up.

    python tests/mod_code_test.py
"""
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'recomp')]
import mod_abi  # noqa: E402
import mod_code  # noqa: E402
import patches  # noqa: E402

MODS = [('main', 0x7000000000, 0x10000), ('sdk', 0x7000200000, 0x10000)]
BASE = """set xc2a XC2_A 1
set xc2b XC2_B 0
site xc2a main 100 d503201f d503201f
site xc2b main 200 d503201f 52800000
spin main 300 310
"""


class TableTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def table(self, text):
        path = Path(self.tmp.name) / 'patches.txt'
        path.write_text(text)
        return patches.load(path, MODS)

    def test_the_same_table_changes_nothing(self):
        self.assertEqual(mod_code.changed_sites(self.table(BASE), self.table(BASE), 'main'), [])

    def test_mod_sites_are_changes_and_only_add(self):
        new = self.table(BASE + 'set mod_1 M 0\nsite mod_1 main 100 d503201f 52800020\n'
                                'site mod_1 main 400 d503201f 52800020\n')
        base = self.table(BASE)
        self.assertEqual(mod_code.changed_sites(base, new, 'main'), [0x100, 0x400])
        self.assertTrue(mod_code.new_only_adds(base, new, 'main', 0x100))
        self.assertTrue(mod_code.new_only_adds(base, new, 'main', 0x400))
        self.assertEqual(mod_code.changed_sites(base, new, 'sdk'), [])

    def test_an_edited_site_of_the_port_runs_always(self):
        base, new = self.table(BASE), self.table(BASE.replace('52800000', '52800040'))
        self.assertEqual(mod_code.changed_sites(base, new, 'main'), [0x200])
        self.assertFalse(mod_code.new_only_adds(base, new, 'main', 0x200))

    def test_a_removed_site_and_a_changed_spin_wait_are_changes(self):
        base = self.table(BASE)
        self.assertEqual(mod_code.changed_sites(base, self.table(BASE.replace('site xc2b main 200 d503201f 52800000\n', '')),
                                                'main'), [0x200])
        self.assertEqual(mod_code.changed_sites(base, self.table(BASE.replace('310', '320')), 'main'), [0x310, 0x320])

    def test_the_builds_sets_keep_their_numbers(self):
        base = self.table(BASE)
        # the mods' sets come first here, and a set the build has is gone
        new = self.table('set mod_2 M2 0\nset xc2b XC2_B 0\nset mod_1 M1 1\n')
        numbers, added = mod_code.number_sets(base, new)
        self.assertEqual(numbers, [2, 1, 3])
        self.assertEqual([s[0] for s in added], ['mod_2', 'mod_1'])


class PartTest(unittest.TestCase):
    def test_other_functions_are_imports_and_its_own_are_not(self):
        p = mod_code.Part(3)
        p.fns = [(0x1000, 'void f_1000(Cpu *c) {}', [14, 15]), (0x2000, 'void f_2000(Cpu *c) {}', [])]
        p.refs = {0x1000, 0x3000}
        text = p.text()
        self.assertIn('#define f_3000 (mod_imports[0])', text)
        self.assertNotIn('#define f_1000', text)
        self.assertIn('sets_1000[] = {14, 15}', text)
        self.assertIn('{0x2000ull, f_2000, 0, 0}', text)
        self.assertIn(mod_code.GENERATED_MARKER, text)


class FilesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name) / 'work'
        self.work.mkdir()
        (self.work / 'exefs.status').write_text('AAAAAAAA\t1\tcode\tx\t\nBBBBBBBB\t2\tcode\t-\t\n', encoding='utf-8')
        (self.work / 'exefs.data').write_text('# h\nAAAAAAAA\t1\t10\t00\nBBBBBBBB\t2\t20\t00\n', encoding='utf-8')
        (self.work / 'cheats.status').write_text('CCCCCCCC\t3\tcode\t0\t\n', encoding='utf-8')

    def test_a_refused_set_costs_its_own_file_only(self):
        mod_code.refuse(self.work, {'mod_aaaaaaaa'}, 'why')
        self.assertEqual((self.work / 'exefs.status').read_text().splitlines(),
                         ['AAAAAAAA\t1\tunsupported\t-\twhy', 'BBBBBBBB\t2\tcode\t-\t'])
        self.assertEqual((self.work / 'exefs.data').read_text().splitlines(), ['# h', 'BBBBBBBB\t2\t20\t00'])
        self.assertIn('code', (self.work / 'cheats.status').read_text())

    def test_a_refused_cheat(self):
        mod_code.refuse(self.work, {'cheat_cccccccc'}, 'why')
        self.assertEqual((self.work / 'cheats.status').read_text(), 'CCCCCCCC\t3\tunsupported\t0\twhy\n')

    def test_new_constant_reads_refuse_data_writes(self):
        base = Path(self.tmp.name) / 'mod-base'
        base.mkdir()
        (base / 'folded.txt').write_text('# x\n100 110\n')
        exefs = Path(self.tmp.name) / 'exefs'
        exefs.mkdir()
        (exefs / 'main').write_bytes(bytes(0x40))
        args = SimpleNamespace(cache=Path(self.tmp.name), exefs=exefs)
        mod_code.recheck_reads(args, self.work, [(0x100, 0x104), (0x40, 0x44)])   # the second one is new; nothing writes it
        self.assertEqual((self.work / 'exefs.status').read_text().count('unsupported'), 0)
        mod_code.recheck_reads(args, self.work, [(0x20, 0x21)])
        self.assertEqual((self.work / 'exefs.status').read_text().splitlines()[0], 'AAAAAAAA\t1\tcode\tx\t')
        self.assertTrue((self.work / 'exefs.status').read_text().splitlines()[1].startswith('BBBBBBBB\t2\tunsupported'))
        self.assertNotIn('BBBBBBBB', (self.work / 'exefs.data').read_text())

    def test_without_sets_drops_their_lines(self):
        f = self.work / 'p.txt'
        f.write_text('set a A 0\nset mod_1 M 0   # x\nsite mod_1 main 10 d503201f d503201f\nsite a main 10 d503201f 0\n')
        mod_code.without_sets(f, {'mod_1'})
        self.assertEqual(f.read_text().splitlines(), ['set a A 0', 'site a main 10 d503201f 0'])

    def test_an_interrupted_publish_leaves_no_current_txt(self):
        cache = Path(self.tmp.name) / 'cache'
        (cache / 'mods').mkdir(parents=True)
        (cache / 'mods' / 'current.txt').write_text('old')
        (self.work / 'exefs.data').unlink()   # the third file is missing: the run stops between
        with self.assertRaises(OSError):
            mod_code.publish(SimpleNamespace(cache=cache), self.work, 'key', None)
        self.assertFalse((cache / 'mods' / 'current.txt').exists())


class AbiTest(unittest.TestCase):
    def test_the_key_follows_headers_generator_and_table(self):
        with tempfile.TemporaryDirectory() as d:
            runtime, base, recomp = (Path(d) / n for n in ('runtime', 'base', 'recomp'))
            for p in (runtime, base, recomp):
                p.mkdir()
            files = [runtime / 'cpu.h', base / 'patches.txt', base / 'folded.txt', recomp / 'gen.py']
            for f in files:
                f.write_bytes(b'a\nb\n')
            keys = {mod_abi.mod_abi(runtime, base, recomp)}
            files[0].write_bytes(b'a\r\nb\r\n')   # line ends alone do not count
            self.assertEqual({mod_abi.mod_abi(runtime, base, recomp)}, keys)
            for f in files:
                f.write_bytes(b'c\n')
                keys.add(mod_abi.mod_abi(runtime, base, recomp))
            self.assertEqual(len(keys), 5)


if __name__ == '__main__':
    unittest.main()
