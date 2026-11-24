"""AMD FidelityFX SDK 1.1.4 for the FSR built into the game (runtime/vk_ffx.cpp): the sources, our patch, the shaders.

ensure() returns toolchain/ffx-sdk-<key>/, made on first use by build_game.py where it compiles the runtime: the folders
of SPARSE at the pinned commit of AMD's repository (git, network; git checks the content against the commit), with
tools/ffx_sdk.patch applied, and spirv/ with the shaders tools/ffx_shaders.py compiles from its GLSL (glslangValidator).
The key covers the commit, the patch and the shader script, so changing one makes a new folder. The setup on the player's
machine links the release's runtime library and needs none of this.

The patch holds the changes of Savage Wind Recomp and TotK Recomp, which brought these effects to other platforms, and
ours; each is marked in it with its origin:

- ffx_vk.cpp: the arrays in the backend's scratch memory are aligned to 64 bytes (with 4, clang's aligned SSE stores
  faulted); the descriptor pool has all six of the sizes it lists and is sized for the passes of these effects, not for
  over two million descriptors of each type; memory types of protected or device-coherent memory, which the runtime does
  not enable, are skipped (RADV lists them last, and the images in them held noise), and a device without device-local
  memory that is not host visible takes such memory.
- ffx_types.h: twice the room for a context outside Windows, where wchar_t is wider, and 8 frames in the backend's rings
  instead of 4 (frame interpolation records twice a frame, with two frames in flight).
- The luma history is declared rgba16f, its images' format; the interpolation's output rgba8, as Intel's GPUs before Arc
  cannot read storage images without a format; the history can be reprojected with a 5-sample Catmull-Rom filter.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import ffx_shaders

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK'
COMMIT = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'   # tag v1.1.4
PATCH = ROOT / 'tools' / 'ffx_sdk.patch'
SPARSE = ['sdk/include/FidelityFX/host', 'sdk/include/FidelityFX/gpu', 'sdk/src/shared', 'sdk/src/backends/shared',
          'sdk/src/backends/vk', 'sdk/src/components/frameinterpolation', 'sdk/src/components/fsr3upscaler',
          'sdk/src/components/opticalflow']
SOURCES = ['sdk/src/shared/*.cpp', 'sdk/src/backends/vk/*.cpp', 'sdk/src/components/*/*.cpp']
INCLUDES = ['sdk/include', 'sdk/src/backends/shared', 'spirv']   # of vk_ffx.cpp and the SDK's sources


def path():
    h = hashlib.sha256(COMMIT.encode())
    for p in (PATCH, Path(ffx_shaders.__file__)):
        h.update(p.read_bytes().replace(b'\r\n', b'\n'))
    return ROOT / 'toolchain' / f'ffx-sdk-{h.hexdigest()[:12]}'


def git(d, *args, stdin=None):
    try:
        r = subprocess.run(['git', '-C', str(d), *args], capture_output=True, input=stdin)
    except FileNotFoundError:
        sys.exit('ffx_sdk: git not found; it fetches the FidelityFX SDK for the built-in FSR')
    if r.returncode:
        sys.exit(f'ffx_sdk: git {args[0]} failed ({URL} @ {COMMIT[:12]}):\n{r.stderr.decode(errors="replace").strip()}')


def remove(d):
    def writable(f, name, _):   # git makes its objects read-only, which Windows does not delete
        os.chmod(name, 0o700)
        f(name)
    shutil.rmtree(d, onexc=writable)


def make(d, glslang):
    git(d, 'init', '-q')
    for key, value in (('core.longpaths', 'true'), ('core.autocrlf', 'false')):
        git(d, 'config', key, value)
    (d / '.git' / 'info').mkdir(exist_ok=True)
    (d / '.git' / 'info' / 'attributes').write_text('* -text\n')   # the files as committed, with LF
    git(d, 'remote', 'add', 'origin', URL)
    git(d, 'sparse-checkout', 'set', '--cone', *SPARSE)
    git(d, 'fetch', '-q', '--depth', '1', '--filter=blob:none', 'origin', COMMIT)
    git(d, 'checkout', '-q', 'FETCH_HEAD')
    git(d, 'apply', '-', stdin=PATCH.read_bytes().replace(b'\r\n', b'\n'))
    ffx_shaders.build(d / 'sdk', d / 'spirv', glslang)
    remove(d / '.git')
    (d / 'spirv' / '.ready').write_text('')


def ensure(glslang=None):
    """The SDK's folder, fetched, patched and with its shaders compiled. glslang: the glslangValidator to use."""
    dst = path()
    if (dst / 'spirv' / '.ready').is_file():
        return dst
    print(f'ffx_sdk: FidelityFX SDK 1.1.4 into {dst.relative_to(ROOT)}', flush=True)
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix='ffx-sdk-', dir=dst.parent))
    try:
        make(tmp, glslang)
        try:
            os.rename(tmp, dst)
        except OSError:   # another build made it meanwhile
            if not (dst / 'spirv' / '.ready').is_file():
                raise
    finally:
        if tmp.is_dir():
            remove(tmp)
    return dst


def sources(sdk):
    return sorted(p for pat in SOURCES for p in sdk.glob(pat))


if __name__ == '__main__':
    print(ensure(sys.argv[1] if len(sys.argv) > 1 else None))
