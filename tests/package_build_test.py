"""Exercise portable packaging without game data or a Windows toolchain."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools' / 'package_build.py'
spec = importlib.util.spec_from_file_location('package_build', SCRIPT)
pkg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pkg)


class PortableTest(unittest.TestCase):
    def test_minimal_xc2_and_optional_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build, extracted, xlate, out = [root / n for n in ('compiled', 'extracted', 'translator', 'portable')]
            for p in (build, extracted / 'exefs', xlate):
                p.mkdir(parents=True)
            for name in (f'xc2{pkg.EXE}', 'romfs-index.bin', *pkg.DLLS, *pkg.OPTIONAL_DLLS):
                (build / name).write_bytes(b'fixture')
            (extracted / 'exefs' / 'main').write_bytes(b'fixture')
            translator = xlate / f'xc2_xlate{pkg.EXE}'
            translator.write_bytes(b'fixture')
            cmd = [sys.executable, str(SCRIPT), '--build', str(build), '--extracted', str(extracted),
                   '--xlate', str(translator), '--out', str(out)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((out / 'exefs' / 'main').is_file())
            self.assertTrue((out / 'profile').is_dir())
            self.assertFalse((out / 'shared-fonts').exists())
            for name in pkg.OPTIONAL_DLLS:
                self.assertEqual((out / name).read_bytes(), b'fixture')
            fonts = extracted.parent / 'firmware' / 'shared-fonts'
            fonts.mkdir(parents=True)
            (fonts / 'font.bin').write_bytes(b'optional font fixture')
            (build / 'romfs-patches').mkdir()
            (build / 'romfs-patches' / 'patch.bin').write_bytes(b'optional patch fixture')
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((out / 'shared-fonts' / 'font.bin').read_bytes(), b'optional font fixture')
            self.assertEqual((out / 'romfs-patches' / 'patch.bin').read_bytes(), b'optional patch fixture')
            # A source nested in the output or the reverse must fail before copying.
            for unsafe in (build, build / 'nested', root):
                result = subprocess.run(cmd[:-1] + [str(unsafe)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('overlap', result.stderr)


if __name__ == '__main__':
    unittest.main()
