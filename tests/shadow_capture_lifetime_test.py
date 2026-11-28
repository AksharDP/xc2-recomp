"""Exercise actual shadow capture and pool finalization with a CPU-only mock device.

No game files or Vulkan device are used. The whole vk_memory.c translation unit is
included, so the test follows its real apply_shadow, shadow_put and vk_pool_fini
branches. Unused external services fail immediately if called.
"""
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ShadowCaptureLifetimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        def missing(message):
            if os.environ.get('XC2_TEST_REQUIRE_DEPS') == '1':
                raise AssertionError(message)
            raise unittest.SkipTest(message)

        if platform.machine().lower() not in ('amd64', 'x86_64'):
            missing('x86-64 host required for the fail-fast service stubs')

        default = 'C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else 'clang'
        cls.cc = shutil.which(os.environ.get('CC', default))
        if not cls.cc:
            missing('Clang required')
        cls.nm = shutil.which(str(Path(cls.cc).with_name('nm.exe' if os.name == 'nt' else 'nm')))
        if not cls.nm:
            cls.nm = shutil.which('nm')
        if not cls.nm:
            missing('nm required')
        include = Path(cls.cc).parent.parent / 'include'
        vma_env = os.environ.get('VMA_INCLUDE_DIR')
        cls.vma = next((p for p in ([Path(vma_env)] if vma_env else []) +
                       [include / 'vma', include, Path('/usr/include')]
                        if (p / 'vk_mem_alloc.h').is_file()), None)
        if cls.vma is None:
            missing('Vulkan Memory Allocator headers required')
        vk_env = os.environ.get('VULKAN_INCLUDE_DIR')
        vulkan = next((p for p in ([Path(vk_env)] if vk_env else []) +
                       [include, Path('/usr/include')]
                       if (p / 'vulkan/vulkan.h').is_file()), None)
        if vulkan is None:
            missing('Vulkan headers required')
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix='shadow-lifetime-', dir=build)
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        obj, stub, exe = out / 'fixture.o', out / 'unused.s', out / 'fixture.exe'
        cmd = [cls.cc, '-std=gnu17', '-O2', '-ffunction-sections', '-fdata-sections',
               '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
               '-I', str(ROOT / 'runtime'), '-I', str(cls.vma), '-I', str(vulkan), '-c',
               str(ROOT / 'tests/shadow_capture_lifetime_fixture.c'), '-o', str(obj)]
        built = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        symbols = subprocess.run([cls.nm, '-u', str(obj)], capture_output=True,
                                 text=True, check=True).stdout
        # The whole translation unit also references services on branches this
        # fixture does not use. On x86-64, fail-fast assembly stubs satisfy those
        # symbols without linking a real device or duplicating runtime functions.
        names = [line.split()[-1] for line in symbols.splitlines() if line.split()]
        services = [n for n in names if re.match(
            r'^(?:vk|vma|rc_vk|cfg_|perf_|mem_|plat_|kern_|callcap_|nvn_flush_stats$|rt_knob$)', n)]
        assembly = '.text\n' + ''.join(
            f'.globl {name}\n{name}:\n jmp fixture_unexpected\n' for name in services)
        if os.name != 'nt':
            assembly += '.section .note.GNU-stack,\"\",@progbits\n'
        stub.write_text(assembly, encoding='utf-8')
        built = subprocess.run([cls.cc, str(obj), str(stub), '-Wl,--gc-sections', '-o', str(exe)],
                               capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        cls.exe = exe

    def test_staged_copy_is_complete_before_destination_is_destroyed(self):
        run = subprocess.run([str(self.exe), 'staged'], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_live_cpu_copy_does_not_require_a_queue_idle_at_finalization(self):
        run = subprocess.run([str(self.exe), 'live'], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_unused_pool_does_not_wait_for_the_gpu(self):
        run = subprocess.run([str(self.exe), 'unused'], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
