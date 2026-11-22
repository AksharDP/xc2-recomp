"""Experimental baseline builds must fail closed and use a distinct runtime CPU marker."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_game as build


class CpuBuildTest(unittest.TestCase):
    def test_missing_baseline_archive_is_not_replaced_by_system_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(build, 'ROOT', Path(tmp)), patch.object(build, 'DEPS', None), \
                 patch.object(build, 'TOOLCHAIN_BIN', None), patch.object(build, 'WIN', True):
                with self.assertRaisesRegex(SystemExit, 'build_baseline_deps.py'):
                    build.use_baseline_deps()

    def test_cross_toolchain_is_not_silently_accepted(self):
        with patch.object(build, 'TOOLCHAIN_BIN', Path('other-toolchain')):
            with self.assertRaisesRegex(SystemExit, 'MSYS2 UCRT64 Windows'):
                build.use_baseline_deps()

    def test_runtime_marker_must_match_baseline_install(self):
        cc = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        if os.name != 'nt' or not cc.is_file():
            self.skipTest('Windows UCRT64 clang is needed')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            src, exe = tmp / 'marker.c', tmp / 'marker.exe'
            cmd = [str(cc), '-march=x86-64', '-DXC2_CPU_LEVEL=1', '-I', str(ROOT / 'runtime'),
                   str(src), str(ROOT / 'runtime/cpu_check.c'), '-fuse-ld=lld', '-luser32', '-o', str(exe)]
            src.write_text('const int rt_cpu_level_v2=2; int main(void) {return 0;}\n')
            bad = subprocess.run(cmd, capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('rt_cpu_level_v1', bad.stderr)
            src.write_text('const int rt_cpu_level_v1=1; int main(void) {return 0;}\n')
            good = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
