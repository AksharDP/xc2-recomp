"""The releases of tools/xc2_titles.py, how the extraction and the installer tell them apart, and that mods laid out for
either are found. Needs no game files; needs cryptography (tools/vendor/macnativegames/extract_exefs.py) and lz4 (recomp/nso.py).

    python tests/regions_test.py
"""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT))

import cheats  # noqa: E402
import exefs_patches  # noqa: E402
import package_select  # noqa: E402
import xc2_extract  # noqa: E402
import xc2_titles  # noqa: E402
from installer import layout as install_layout  # noqa: E402
from installer import steps  # noqa: E402
from installer.runner import InstallError  # noqa: E402

US, JP = xc2_titles.BY_KEY['us'], xc2_titles.BY_KEY['jp']


def header(bktr=False):
    """An NCA header whose first section is a BKTR patch (encryption type 4 at +0x400 + 4) or plain."""
    h = bytearray(0xC00)
    h[0x404] = 4 if bktr else 3
    return bytes(h)


def rec(title, content_type):
    return {'title_id': title, 'content_type': content_type, 'size': 1}


class TitlesTest(unittest.TestCase):
    def test_ids(self):
        self.assertEqual((US.title, US.update_title), ('0100E95004038000', '0100E95004038800'))
        self.assertEqual((JP.title, JP.update_title), ('0100F3400332C000', '0100F3400332C800'))
        self.assertIs(xc2_titles.of_title('0100f3400332c800'), JP)
        self.assertIsNone(xc2_titles.of_title('0100F2C0115B6000'))

    def test_aoc(self):
        self.assertEqual(xc2_titles.of_aoc('0100E95004039063'), (US, 99))
        self.assertEqual(xc2_titles.of_aoc('0100F3400332D001'), (JP, 1))
        self.assertIsNone(US.aoc_index('0100F3400332D001'))
        self.assertIsNone(xc2_titles.of_aoc('0100E95004038000'))

    def test_names(self):
        self.assertEqual(US.version_tag('3.0.1'), 'v301')
        self.assertEqual(JP.version_tag('3.0.1'), 'v301jp')
        self.assertEqual(US.expected_name('3.0.1'), 'expected-xc2-3.0.1.json')
        self.assertEqual(JP.expected_name('1.0.0'), 'expected-xc2-jp-1.0.0.json')
        self.assertIs(xc2_titles.of_tag('v301jp'), JP)
        self.assertIs(xc2_titles.of_tag('v301'), US)
        self.assertTrue(US.expected('3.0.1').is_file() and US.expected('1.0.0').is_file())

    def test_patch_tables(self):
        self.assertEqual(US.patch_table('3.0.1'), 'patches-v301.txt')
        self.assertEqual(JP.patch_table('3.0.0'), 'patches-v301.txt')
        self.assertEqual(JP.patch_table('3.0.1'), 'patches-v301jp.txt')   # no such update: no table is shared
        self.assertTrue(JP.expected('3.0.0').is_file())
        self.assertTrue((ROOT / 'recomp' / JP.patch_table('3.0.0')).is_file())


class SelectTest(unittest.TestCase):
    def test_base_of_either_release(self):
        for r in (US, JP):
            records = [(rec(r.title, 1), header()), (rec(r.title, 0), header())]
            self.assertEqual(xc2_extract.program_nca(records)[0]['title_id'], r.title)

    def test_update_of_either_release(self):
        for r in (US, JP):
            records = [(rec(r.update_title, 1), header()), (rec(r.title, 0), header(bktr=True))]
            self.assertEqual(xc2_extract.program_nca(records, update=True)[0]['title_id'], r.title)

    def test_mixed_releases_refused(self):
        with self.assertRaises(ValueError):
            xc2_extract.program_nca([(rec(US.title, 1), header()), (rec(JP.title, 0), header())])
        with self.assertRaises(ValueError):
            xc2_extract.program_nca([(rec(JP.update_title, 1), header()), (rec(US.title, 0), header(bktr=True))],
                                    update=True)

    def test_combined_cartridge(self):
        records = [(rec(JP.title, 1), header()), (rec(JP.title, 0), header()),
                   (rec(JP.update_title, 1), header()), (rec(JP.title, 0), header(bktr=True))]
        self.assertEqual(package_select.program_nca(records)[0]['title_id'], JP.title)


class InstallerTest(unittest.TestCase):
    def test_supported_releases(self):
        lay = install_layout.find(ROOT)
        self.assertTrue(lay.supports(US))
        self.assertEqual(lay.expected('3.0.1', JP).name, 'expected-xc2-jp-3.0.1.json')

    def test_release_versions_come_from_data(self):
        import dataclasses
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / 'data'
            data.mkdir()
            for name in ('expected-xc2-1.0.0.json', 'expected-xc2-3.0.1.json', 'expected-xc2-jp-1.0.0.json',
                         'expected-xc2-jp-2.1.0.json'):
                (data / name).write_text('{}')
            lay = dataclasses.replace(install_layout.find(ROOT), tools=Path(tmp))
            self.assertEqual(lay.versions(US), ['3.0.1'])
            self.assertEqual(lay.versions(JP), ['2.1.0'])
            self.assertTrue(lay.supports(JP))
            (data / 'expected-xc2-jp-1.0.0.json').unlink()
            self.assertFalse(lay.supports(JP))

    def test_dlc_of_another_release(self):
        class Fake:
            def stat(self):
                return type('S', (), {'st_size': 1})()

        saved = steps.container_entries, steps.nca_inventory
        try:
            steps.container_entries = lambda f, size: ('nsp', [])
            steps.nca_inventory = lambda f, entries, keys: [(rec('0100F3400332D001', 5), header())]
            self.assertEqual(steps.dlc_record(None, Fake(), {}, JP)[0], '0100F3400332D001')
            with self.assertRaises(InstallError) as e:
                steps.dlc_record(None, Fake(), {}, US)
            self.assertEqual(e.exception.id, 'check.dlc_other_region')
        finally:
            steps.container_entries, steps.nca_inventory = saved


class ModPlacesTest(unittest.TestCase):
    """The tools that build the mods' code at game start (tools/mod_code.py runs both) find a mod's files in its own
    folder and in the Atmosphere and emulator layouts of either release, as find_root of runtime/mods.c does for the
    release of the build."""

    def check_places(self, part, name, find):
        """find(mods folder) gives (mod name, path in the mod) of the one file of a mod that has <part>/<name> in each
        place in turn."""
        for where in ('', *(f'{w}{r.title}/' for r in (US, JP) for w in ('atmosphere/contents/', 'contents/', ''))):
            place = f'{where}{part}/{name}'
            with self.subTest(place=place), tempfile.TemporaryDirectory() as tmp:
                f = Path(tmp) / 'Mod' / place
                f.parent.mkdir(parents=True)
                f.write_bytes(b'')
                self.assertEqual(find(Path(tmp)), [('Mod', place)])

    def test_cheats(self):
        bid = '0123456789ABCDEF'
        self.check_places('cheats', f'{bid}.txt', lambda mods: [
            (mod, f.relative_to(mods / mod).as_posix()) for mod, f in cheats.cheat_files(mods, bid)])

    def test_exefs_patches(self):
        self.check_places('exefs', 'a.ips', lambda mods: [
            (mod, place) for mod, place, _ in exefs_patches.patch_files(mods)])


if __name__ == '__main__':
    unittest.main()
