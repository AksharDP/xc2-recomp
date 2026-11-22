"""Build experimental Windows baseline x86-64 VP9/Zstandard archives.

These archives do not make the complete game baseline-compatible. Audit the final executable and its DLLs and run
older-CPU tests before claiming support. Uses the documented MSYS2 UCRT64 toolchain, plus nasm, diffutils and perl.
Downloads public source into ignored toolchain/src; writes libraries to ignored toolchain/baseline/lib.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ('zstd-baseline', 'https://github.com/facebook/zstd.git', 'v1.5.7',
     'f8745da6ff1ad1e7bab384bd1f9d742439278e99', 'lib/libzstd.a'),
    ('libvpx-baseline', 'https://github.com/webmproject/libvpx.git', 'v1.16.0',
     '1024874c5919305883187e2953de8fcb4c3d7fa6', 'libvpx.a'),
)


def run(cmd, **kwargs):
    subprocess.run(cmd, check=True, **kwargs)


def main():
    if os.name != 'nt':
        sys.exit('build_baseline_deps: this experiment targets Windows UCRT64')
    bash = Path(r'C:\msys64\usr\bin\bash.exe')
    if not bash.is_file():
        sys.exit('build_baseline_deps: missing MSYS2 bash')
    for name, url, tag, pin, archive in SOURCES:
        src = ROOT / 'toolchain' / 'src' / name
        src.parent.mkdir(parents=True, exist_ok=True)
        if not src.exists():
            run(['git', 'clone', '--depth', '1', '--branch', tag, url, str(src)])
        actual = subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != pin:
            sys.exit(f'build_baseline_deps: {name}: expected {pin}, found {actual}; source left unchanged')
        run(['git', '-C', str(src), 'diff', '--quiet', 'HEAD'])
        # With cwd=ROOT, MSYS2 translates the working directory, including spaces, without shell interpolation.
        script = 'export PATH=/ucrt64/bin:/usr/bin:$PATH; export CC=clang CXX=clang++ AR=llvm-ar; '
        if name == 'zstd-baseline':
            script += ("make -C toolchain/src/zstd-baseline/lib clean && "
                       "make -C toolchain/src/zstd-baseline/lib -j8 CC=clang AR=llvm-ar CFLAGS='-O2 -march=x86-64' libzstd.a")
        else:
            # Discard objects from an earlier invocation with different flags; make does not key them by CFLAGS.
            script += ("cd toolchain/src/libvpx-baseline && if test -f Makefile; then make clean; fi; "
                       "./configure --target=x86_64-win64-gcc "
                       "--disable-vp8 --disable-vp9-encoder --disable-examples --disable-tools --disable-docs "
                       "--disable-unit-tests --enable-runtime-cpu-detect --extra-cflags='-O2 -march=x86-64' "
                       "--extra-cxxflags='-O2 -march=x86-64' && make -j8")
        run([str(bash), '-e', '-c', script], cwd=ROOT)
        out = ROOT / 'toolchain' / 'baseline' / 'lib' / Path(archive).name
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src / archive, out)
        print(f'build_baseline_deps: {name} at {pin}: {out}', flush=True)


if __name__ == '__main__':
    main()
