"""Where the installer finds what it runs: the one place that maps the release folder and a repository checkout.

Release (docs/installer.md "Release layout"), root = the unpacked folder:

    python/python.exe       runs every tool (numpy, lz4, cryptography, compression.zstd); its ._pth puts .., ../tools
                            and ../recomp on sys.path, so `python -m installer` imports from the release folder
    recomp/, tools/         the lifter and the scripts; tools/data/expected-xc2[-jp]-<version>.json for the base game
                            and each supported update (SUPPORTED) of each release (REGIONS)
    llvm/                   llvm-mingw (bin/clang.exe)
    runtime/                libxc2rt.a, the headers, install-side sources (romfs.c, patches.c, ov_zstd.c) and the icon; runtime/deps/ holds
                            include/ + lib/ of Vulkan, SDL3, Opus, libvpx and VMA (build_game.py --deps)
    bin/                    xc2_xlate.exe (+ xc2_xlate.id), ninja.exe, SDL3.dll, libopus-0.dll, amd_fidelityfx_vk.dll
    xc2.ini.example
    VERSION                 the release version, one line
    temp/                   the default temp folder (created)

Linux release (the AppImage's folder, docs/installer.md "Linux release"): python/bin/python3 (python-build-standalone,
with tkinter), llvm/ with clang, lld and llvm-ar plus llvm/sysroot (the glibc headers and libraries the game links
against), runtime/deps/ with include/ and the static Opus, libvpx and zstd, bin/ without .exe and with libSDL3.so.0.

Dev mode, root = a repository checkout (no python/python.exe): build/py314-embed/py/python.exe (the embeddable
Python the release ships, with the same packages), else this Python; toolchain/llvm-mingw-*/ and toolchain/deps-mingw;
runtime/ as the sources with build/install-rtlib/libxc2rt.a (tools/build_game.py --make-runtime-lib --out
build/install-rtlib); MSYS2 UCRT64 ninja.exe; build/xlate/xc2_xlate.exe; build/install-temp.
Without toolchain/, the tools come from an unpacked XCX Recomp release folder, the one tools/build_xcx.ps1 builds
with (TOOLS_FALLBACK). The version is VERSION with -dev and the commit: 0.1.0-dev+973c626. On Linux,
dev mode takes clang, lld and ninja from PATH and the system's libraries (no deps folder, no SDL3 copy).
"""
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

WIN = os.name == 'nt'
EXE = '.exe' if WIN else ''
UCRT = Path(r'C:\msys64\ucrt64\bin')
SUPPORTED = ('3.0.1',)   # updates the installer builds; the check identifies the player's by hash
BASE = '1.0.0'           # the base game's version, checked by hash too
# The releases (tools/xc2_titles.py): the Americas and Europe share one title, Japan has its own. The check tells them
# apart by the base game's title id and builds a release only when tools/data holds its expected files.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import xc2_titles  # noqa: E402
REGIONS, DEFAULT_REGION = xc2_titles.REGIONS, xc2_titles.DEFAULT
GAME_EXE = 'xc2' + EXE   # the game's name in the install folder (the same name the runtime and build give it)
LEGACY_EXE = 'xcx' + EXE   # the same name in an install folder made before the rename to XC2 (steps.py removes it)
APP_ID = DEFAULT_REGION.app_id   # Xenoblade Chronicles 2 in the Americas and Europe; Region.app_id for the others


def tools_fallback():
    """An unpacked XCX-Setup release folder whose python/, llvm/ and bin/ dev mode borrows when the repository has no
    toolchain of its own: XC2_SETUP_TOOLS, else the newest XCX-Setup-* under ../tools/xcx-setup beside the
    repository."""
    env = os.environ.get('XC2_SETUP_TOOLS')
    if env:
        return Path(env)
    found = sorted((Path(__file__).resolve().parent.parent.parent / 'tools' / 'xcx-setup').glob('XCX-Setup-*'))
    return found[-1] if found else Path('XC2_SETUP_TOOLS-not-set')


def version_tag(version, region=DEFAULT_REGION):
    """'3.0.1' -> 'v301' ('v301jp' for Japan): the version name of build_game.py (its generated/ and build/ folders)."""
    return region.version_tag(version)


@dataclass(frozen=True)
class Layout:
    root: Path
    dev: bool
    release: str
    python: Path          # runs the tools
    tools: Path
    recomp: Path
    llvm: Path            # llvm-mingw tree; on Linux the tree with bin/clang (a release's, or the system's /usr)
    deps: Path | None     # include/ and lib/ for the link; None: the system's (Linux dev mode)
    runtime: Path         # runtime headers and install-side sources
    runtime_lib: Path     # libxc2rt.a
    ninja: Path
    xlate: Path           # xc2_xlate.exe; a xc2_xlate.id beside it is copied too
    dlls: tuple = field(default_factory=tuple)
    ini_example: Path | None = None
    temp: Path | None = None
    sysroot: Path | None = None       # Linux release: what the game links against, instead of the host's

    @property
    def clang(self):
        return self.llvm / 'bin' / ('clang' + EXE)

    def expected(self, version, region=DEFAULT_REGION):
        """The expected hashes of the base game (BASE) or an update of a release."""
        return region.expected(version, self.tools / 'data')

    def versions(self, region=DEFAULT_REGION):
        """The updates of a release the installer builds: the ones tools/data holds expected files for (SUPPORTED for
        the Americas and Europe; another release's numbering may differ)."""
        found = []
        for path in (self.tools / 'data').glob(region.expected_name('*')):
            v = path.name[len(region.expected_name('')) - len('.json'):-len('.json')]
            if v != BASE and v[:1].isdigit() and path.name == region.expected_name(v):
                found.append(v)
        return sorted(found, key=lambda v: tuple(int(p) for p in v.split('.') if p.isdigit()))

    def supports(self, region):
        """1 when the release's base game and an update can be built: tools/data holds their expected files."""
        return self.expected(BASE, region).is_file() and bool(self.versions(region))

    def missing(self):
        """Paths the steps need that do not exist."""
        need = [self.python, self.tools / 'extract.py', self.tools / 'build_game.py', self.recomp / 'gen.py',
                self.clang,
                *((self.deps / 'include', self.deps / 'lib') if self.deps else ()), self.runtime, self.runtime_lib, self.ninja,
                self.xlate, *self.dlls, *(self.expected(v) for v in (BASE, *SUPPORTED))]
        if self.sysroot:
            need.append(self.sysroot)
        return [str(p) for p in need if not p.exists()]


def release_python(root):
    return root / 'python' / ('python.exe' if WIN else 'bin/python3')


def is_release(root):
    return release_python(root).is_file()


def release_translator(root):
    """The shader translator of an unpacked release: xc2_xlate, or the xcx_xlate that an XCX-Setup release names it
    (tools_fallback may point at one)."""
    bin_dir = root / 'bin'
    for stem in ('xc2_xlate', 'xcx_xlate'):
        if (bin_dir / (stem + EXE)).is_file():
            return bin_dir / (stem + EXE)
    return bin_dir / ('xc2_xlate' + EXE)


def release_dlls(root):
    """The DLLs xc2.exe loads: SDL3 and Opus, the C++ runtime of the llvm-mingw it is linked with, and AMD's FSR 3.1
    library. That one only when present: the game runs without it, and an older setup used as tools_fallback lacks it."""
    fsr = root / 'bin' / 'amd_fidelityfx_vk.dll'
    return (root / 'bin' / 'SDL3.dll', root / 'bin' / 'libopus-0.dll',
            *(root / 'llvm' / 'bin' / n for n in ('libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll')),
            *((fsr,) if fsr.is_file() else ()))


def release_layout(root):
    version = root / 'VERSION'
    return Layout(
        root=root, dev=False,
        release=version.read_text(encoding='utf-8').strip() if version.is_file() else 'unknown',
        python=release_python(root),
        tools=root / 'tools', recomp=root / 'recomp',
        llvm=root / 'llvm', deps=root / 'runtime' / 'deps', runtime=root / 'runtime',
        runtime_lib=root / 'runtime' / 'libxc2rt.a',
        ninja=root / 'bin' / ('ninja' + EXE),
        xlate=root / 'bin' / ('xc2_xlate' + EXE), dlls=release_dlls(root) if WIN else (root / 'bin' / 'libSDL3.so.0',),
        ini_example=root / 'xc2.ini.example',
        # Linux: None, the install folder's .setup-temp (installer/cli.py): the AppImage is mounted read-only, and on
        # a Steam Deck /home is often too full for the generated C while the install folder is on the SD card
        temp=root / 'temp' if WIN else None,
        sysroot=None if WIN else root / 'llvm' / 'sysroot')


def dev_layout(root):
    if not WIN:
        return dev_layout_posix(root)
    commit = subprocess.run(['git', '-C', str(root), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                            text=True).stdout.strip()
    embed = root / 'build' / 'py314-embed' / 'py' / ('python' + EXE)
    trees = sorted(p.parent.parent for p in (root / 'toolchain').glob('llvm-mingw-*/llvm-mingw-*/bin/clang' + EXE))
    deps = root / 'toolchain' / 'deps-mingw'
    semver = (root / 'VERSION').read_text(encoding='utf-8').strip()
    tools = tools_fallback()
    if not trees and is_release(tools):   # no toolchain/: the tools of an unpacked XCX-Setup release
        return Layout(
            root=root, dev=True, release=f'{semver}-dev+{commit or "unknown"}',
            python=tools / 'python' / ('python' + EXE),
            tools=root / 'tools', recomp=root / 'recomp',
            llvm=tools / 'llvm', deps=tools / 'runtime' / 'deps', runtime=root / 'runtime',
            runtime_lib=root / 'build' / 'install-rtlib' / 'libxc2rt.a',
            ninja=tools / 'bin' / ('ninja' + EXE),
            xlate=release_translator(tools), dlls=release_dlls(tools),
            ini_example=root / 'xc2.ini.example', temp=root / 'build' / 'install-temp')
    return Layout(
        root=root, dev=True, release=f'{semver}-dev+{commit or "unknown"}',
        python=embed if embed.is_file() else Path(sys.executable),
        tools=root / 'tools', recomp=root / 'recomp',
        llvm=trees[-1] if trees else root / 'toolchain' / 'llvm-mingw-missing', deps=deps,
        runtime=root / 'runtime', runtime_lib=root / 'build' / 'install-rtlib' / 'libxc2rt.a',
        ninja=UCRT / ('ninja' + EXE),
        xlate=root / 'build' / 'xlate' / ('xc2_xlate' + EXE),
        dlls=tuple(sorted((deps / 'bin').glob('*.dll'))),
        ini_example=root / 'xc2.ini.example', temp=root / 'build' / 'install-temp')


def dev_commit(root):
    return subprocess.run(['git', '-C', str(root), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                          text=True).stdout.strip()


def dev_layout_posix(root):
    clang, ninja = shutil.which('clang'), shutil.which('ninja')
    semver = (root / 'VERSION').read_text(encoding='utf-8').strip()
    return Layout(
        root=root, dev=True, release=f'{semver}-dev+{dev_commit(root) or "unknown"}',
        python=Path(sys.executable), tools=root / 'tools', recomp=root / 'recomp',
        llvm=Path(clang).parent.parent if clang else root / 'toolchain' / 'clang-missing', deps=None,
        runtime=root / 'runtime', runtime_lib=root / 'build' / 'install-rtlib' / 'libxc2rt.a',
        ninja=Path(ninja) if ninja else root / 'toolchain' / 'ninja-missing',
        xlate=root / 'build' / 'xlate' / 'xc2_xlate',
        ini_example=root / 'xc2.ini.example', temp=root / 'build' / 'install-temp')


def find(root=None):
    """The layout of root (default: the folder above this package)."""
    root = Path(root or Path(__file__).resolve().parent.parent).resolve()
    return release_layout(root) if is_release(root) else dev_layout(root)
