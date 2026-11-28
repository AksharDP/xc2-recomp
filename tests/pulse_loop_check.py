"""Check XC2's additive UI loop against an analytic synthetic animation.

Requires the user's extracted Western 3.0.1 ExeFS, Unicorn and the lifter's
dependencies. No game instructions or assets are included in this test. It runs
the actual controller, key decoder, sampler and pane setters from that ExeFS.

Usage: python tests/pulse_loop_check.py --exefs build/extracted/v301/exefs
All derived output is written below build/pulse-loop-check.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import capstone
import unicorn as uc
from unicorn import arm64_const as arm

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from gen import Emitter, chunk_text  # noqa: E402
from layout import layout  # noqa: E402
from module import Module  # noqa: E402
import patches  # noqa: E402

# Western 3.0.1 functions, with exclusive ends. The content hash checks in the
# patch table reject a different executable before any instructions are run.
FUNCTIONS = (
    (0x66C7AC, 0x66C9D8),  # animation controller
    (0x670AD4, 0x670C74),  # additive component
    (0x67DDC4, 0x67DE60),  # asset duration
    (0x67DE60, 0x67DEB4),  # initial key
    (0x67DEB4, 0x67E324),  # key decoder
    (0x67E324, 0x67E504),  # additive sampler
    (0x695AA4, 0x695ACC),  # scale setter
    (0x69033C, 0x690390),  # position setter
    (0x695A68, 0x695AA4),  # position update
)


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


class Animation:
    """Actual guest instructions with a synthetic asset and two small UI stubs."""

    HEAP = 0x7F00000000
    STACK = HEAP + 0x20000
    SCRATCH = HEAP + 0x40000
    STOP = HEAP + 0x60000
    GET_POSITION = STOP + 0x100
    OBSERVE_CONTEXT = STOP + 0x200

    def __init__(self, mod, replacements):
        self.mod = mod
        self.mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        self.mu.mem_map(mod.base, len(mod.image))
        self.mu.mem_write(mod.base, mod.image)
        self.mu.mem_map(self.HEAP, 0x20000)
        self.mu.mem_map(self.STACK, 0x10000)
        self.mu.mem_map(self.SCRATCH, 0x10000)
        self.mu.mem_map(self.STOP, 0x1000)
        self.mu.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 0x300000)
        self.redirect, self.returns = {}, {}
        self.install(replacements)
        self.mu.hook_add(uc.UC_HOOK_CODE, self.on_code)
        self.controller = self.HEAP
        self.component = self.HEAP + 0x1000
        self.pane = self.HEAP + 0x3000
        self.context = None
        self.context_size = 16 if replacements else 12
        self.make_asset()

    def put64(self, address, value):
        self.mu.mem_write(address, struct.pack('<Q', value))

    def put32(self, address, value):
        self.mu.mem_write(address, struct.pack('<I', value))

    def put_float(self, address, value):
        self.mu.mem_write(address, struct.pack('<f', value))

    def float_at(self, address):
        return struct.unpack('<f', bytes(self.mu.mem_read(address, 4)))[0]

    def u32_at(self, address):
        return struct.unpack('<I', bytes(self.mu.mem_read(address, 4)))[0]

    def install(self, replacements):
        scratch = self.SCRATCH
        disasm = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for address, alternatives in replacements.items():
            # Only xc2pulse is enabled in this isolated animation check.
            words = alternatives[0][1]
            code = b''.join(struct.pack('<I', word) for word in words)
            if len(words) == 1:
                self.mu.mem_write(address, code)
                continue
            # A replacement expands ONE original instruction. In-place writes
            # would overwrite its successors and test a different program.
            for word in words:
                ins = next(disasm.disasm(struct.pack('<I', word), address))
                if ins.mnemonic in {'adr', 'adrp', 'b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz'} or ins.mnemonic.startswith('b.'):
                    raise AssertionError('multiword PC-relative replacement needs explicit relocation')
            self.redirect[address] = scratch
            self.returns[scratch + len(code)] = address + 4
            self.mu.mem_write(scratch, code)
            scratch += 0x100

    def on_code(self, mu, address, size, _):
        if address in self.redirect:
            mu.reg_write(arm.UC_ARM64_REG_PC, self.redirect[address])
        elif address in self.returns:
            mu.reg_write(arm.UC_ARM64_REG_PC, self.returns[address])
        elif address == self.GET_POSITION:
            # The pane's virtual position getter returns origin (0, 0). The
            # actual scale and position setters still execute normally.
            mu.mem_write(mu.reg_read(arm.UC_ARM64_REG_X2), b'\0' * 4)
            mu.reg_write(arm.UC_ARM64_REG_PC, mu.reg_read(arm.UC_ARM64_REG_X30))
        elif address == self.OBSERVE_CONTEXT:
            # A second component checks that the first did not change the
            # controller's shared context, including the pre-advance loop count
            # supplied by the patch. The unpatched seek control has three words.
            self.context = bytes(mu.mem_read(mu.reg_read(arm.UC_ARM64_REG_X2), self.context_size))
            mu.reg_write(arm.UC_ARM64_REG_PC, mu.reg_read(arm.UC_ARM64_REG_X30))
        elif address == self.mod.base + 0x6983F8:
            raise AssertionError('the synthetic animation triggered a game assertion')

    def make_asset(self):
        table, pane_table, asset, channels, frames, keys = (
            self.HEAP + offset for offset in (0x2000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000))
        c, component, pane = self.controller, self.component, self.pane
        self.put64(c + 8, pane)
        self.put_float(c + 0x18, 1.0)  # controller duration
        self.mu.mem_write(c + 0x1C, b'\1')  # looping
        self.put_float(c + 0x80, 1.0)  # playback speed
        self.put64(c + 0x78, 2)  # component count
        self.put64(c + 0x28, component)
        self.put64(component, table)
        self.put64(table, self.mod.base + 0x670AD4)
        self.put64(component + 8, asset)
        self.put_float(component + 0x30, 8.0)  # captured base dimensions
        self.put_float(component + 0x34, 8.0)
        self.put_float(pane + 0x30, 8.0)
        self.put_float(pane + 0x34, 8.0)
        self.put_float(pane + 0x6C, 0.5)
        self.put64(pane, pane_table)
        self.put64(pane_table + 0x38, self.GET_POSITION)
        observer, observer_table = self.HEAP + 0x9000, self.HEAP + 0xA000
        self.put64(c + 0x38, observer)
        self.put64(observer, observer_table)
        self.put64(observer_table, self.OBSERVE_CONTEXT)
        self.put64(asset + 8, channels)
        self.put64(asset + 0x10, 1)
        self.put64(channels, frames)
        self.put64(channels + 8, 31)
        for frame in range(31):
            key = keys + frame * 48
            self.put64(frames + frame * 40, key)
            self.put64(frames + frame * 40 + 8, 6)
            # Real decoder types: 1 mode, 2 x position, 4/5 size, 6 rotation in
            # degrees, 7 alpha in percent. Flags 0 embed each value directly.
            scale = 1.0 if frame == 0 else -0.01 if frame <= 15 else 0.01
            values = struct.pack('<HHI', 1, 0, 0 if frame == 0 else 1)
            for kind, value in ((2, 0.0 if frame == 0 else 0.01), (4, scale), (5, scale),
                                (6, 0.0 if frame == 0 else 0.1), (7, 50.0 if frame == 0 else 2.0)):
                values += struct.pack('<HHf', kind, 0, value)
            self.mu.mem_write(key, values)

    def step(self, delta):
        delta = f32(delta)
        phase = self.float_at(self.controller + 0x10)
        loops = self.u32_at(self.controller + 0x20)
        expected_context = struct.pack('<fff', phase, delta, 1.0)
        if self.context_size == 16:
            expected_context += struct.pack('<I', loops)
        self.context = None
        self.mu.reg_write(arm.UC_ARM64_REG_X0, self.controller)
        self.mu.reg_write(arm.UC_ARM64_REG_S0, struct.unpack('<I', struct.pack('<f', delta))[0])
        self.mu.reg_write(arm.UC_ARM64_REG_SP, self.STACK + 0xF000)
        self.mu.reg_write(arm.UC_ARM64_REG_X30, self.STOP)
        self.mu.emu_start(self.mod.base + 0x66C7AC, self.STOP, count=20000)
        if self.mu.reg_read(arm.UC_ARM64_REG_PC) != self.STOP:
            raise AssertionError('animation exceeded its instruction limit')
        if self.context != expected_context:
            raise AssertionError('a component changed the shared callback context')
        return phase, delta, loops


def check_profile(mod, replacements, name, steps):
    animation = Animation(mod, replacements)
    errors = dict(scale=0.0, position=0.0, rotation=0.0, alpha=0.0)
    radians = struct.unpack_from('<f', mod.image, 0x9B51A4)[0]
    for delta in steps:
        phase, delta, loops = animation.step(delta)
        # The asset has an absolute initial key and 30 additive keys. Each key
        # occupies 1/30 second; the asset's duration is 31/30 second, normalized
        # to this controller's one-second loop. On the crossing frame the
        # original sampler stops at the endpoint; the next callback catches up
        # the new loop's prefix. This oracle is independent of sampler code.
        units = min(max((phase + delta) * 31.0, 0.0), 31.0)
        expected = {
            'scale': 8.0 + 8.0 * (-0.01 * min(units, 15.0) + 0.01 * min(max(units - 15.0, 0.0), 15.0)),
            'position': loops * 0.3 + 0.01 * min(units, 30.0),
            'rotation': (loops * 3.0 + 0.1 * min(units, 30.0)) * radians,
            'alpha': min(1.0, 0.5 + 0.02 * min(units, 30.0)),
        }
        actual = {
            'scale': animation.float_at(animation.pane + 0x30),
            'position': animation.float_at(animation.component + 0x20),
            'rotation': animation.float_at(animation.pane + 0x40),
            'alpha': animation.float_at(animation.pane + 0x6C),
        }
        for channel in errors:
            if not math.isfinite(actual[channel]):
                raise AssertionError(f'{name}: non-finite {channel}: {actual[channel]}')
            errors[channel] = max(errors[channel], abs(actual[channel] - expected[channel]))
    limits = dict(scale=0.0001, position=0.002, rotation=0.0005, alpha=0.00005)
    for channel, error in errors.items():
        if error > limits[channel]:
            raise AssertionError(f'{name}: {channel} error {error} exceeds {limits[channel]}')
    return errors


def scale_at(phase):
    units = min(max(phase * 31.0, 0.0), 31.0)
    return 8.0 + 8.0 * (-0.01 * min(units, 15.0) + 0.01 * min(max(units - 15.0, 0.0), 15.0))


def check_state_changes(mod, replacements):
    # A disabled component must not consume loop epochs while the controller
    # continues advancing. Its first active sample rebuilds the current prefix.
    animation = Animation(mod, replacements)
    animation.put32(animation.component + 0x10, 0xFFFFFFFF)
    for _ in range(300):
        animation.step(1 / 120)
    assert animation.u32_at(animation.component + 0x14) == 0
    assert animation.float_at(animation.pane + 0x30) == 8.0
    animation.put32(animation.component + 0x10, 0)
    phase, delta, loops = animation.step(1 / 120)
    assert animation.u32_at(animation.component + 0x14) == loops
    assert abs(animation.float_at(animation.pane + 0x30) - scale_at(phase + delta)) < 0.0001

    # The start/reset routine main+0x66ca6c clears the controller phase and loop
    # count. Model those state writes without claiming to test asset rebinding.
    animation.put_float(animation.controller + 0x10, 0.0)
    animation.put32(animation.controller + 0x20, 0)
    phase, delta, loops = animation.step(1 / 120)
    assert animation.u32_at(animation.component + 0x14) == 0
    assert abs(animation.float_at(animation.pane + 0x30) - scale_at(delta)) < 0.0001

    # A nonzero seek within the same epoch must retain the original component's
    # behavior. Compare the actual unpatched instructions, including the sampler,
    # rather than defining a new seek policy in the loop patch.
    values = []
    for selected in ({}, replacements):
        sought = Animation(mod, selected)
        sought.put_float(sought.controller + 0x10, 0.37)
        sought.put32(sought.controller + 0x20, 5)
        sought.put32(sought.component + 0x14, 5)
        sought.put_float(sought.pane + 0x30, 9.0)
        sought.step(1 / 120)
        values.append(tuple(sought.float_at(address) for address in
                            (sought.pane + 0x30, sought.component + 0x20,
                             sought.pane + 0x40, sought.pane + 0x6C)))
    assert values[0] == values[1], (values[0], values[1])

    # Epoch comparison uses all 32 bits, including rollover and the sign bit.
    # The adjacent low word remains the asset index throughout actual decoding.
    for current, previous in ((0, 0xFFFFFFFF), (0x80000000, 0), (1, 0x80000000)):
        wrapped = Animation(mod, replacements)
        wrapped.put_float(wrapped.controller + 0x10, 0.03)
        wrapped.put32(wrapped.controller + 0x20, current)
        wrapped.put32(wrapped.component + 0x14, previous)
        phase, delta, loops = wrapped.step(1 / 120)
        assert wrapped.u32_at(wrapped.component + 0x10) == 0
        assert wrapped.u32_at(wrapped.component + 0x14) == current
        assert abs(wrapped.float_at(wrapped.pane + 0x30) - scale_at(phase + delta)) < 0.0001
    return 'inactive, restart state, same-epoch seek, and three epoch transitions passed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, default=ROOT / 'build/extracted/v301/exefs')
    parser.add_argument('--patches', type=Path, default=ROOT / 'recomp/patches-v301.txt')
    args = parser.parse_args()
    modules = layout(args.exefs)
    _, base, _ = next(item for item in modules if item[0] == 'main')
    mod = Module('main', args.exefs / 'main', base, 'c')
    table = patches.load(args.patches, modules)
    patches.check(table, mod, 'main')
    pulse = next(i for i, entry in enumerate(table.sets) if entry[0] == 'xc2pulse')
    replacements = {address: [(index, words) for index, words in alternatives if index == pulse]
                    for address, alternatives in table.for_module('main', base).items()
                    if any(index == pulse for index, words in alternatives)}
    functions = [(base + start, base + end) for start, end in FUNCTIONS]
    emitter = Emitter(mod, {start for start, end in functions}, replacements)
    generated = chunk_text('pulse_check', 0, emitter, functions)
    patch_hash = hashlib.sha256(args.patches.read_bytes()).hexdigest()
    out = ROOT / 'build/pulse-loop-check' / patch_hash[:12]
    out.mkdir(parents=True, exist_ok=True)
    # chunk_text supplies the mandatory generated-game-code marker.
    (out / 'pulse.c').write_text(generated, encoding='utf-8')
    results = dict(patch_sha256=patch_hash, game_build_id=mod.nso.build_id, status='not_completed')
    report = out / 'results.json'
    report.write_text(json.dumps(results, indent=2), encoding='utf-8')
    for name, steps in (('30fps', [1 / 30] * 600), ('120fps', [1 / 120] * 2400),
                        ('hitches', [0.04, 1 / 120, 1 / 120, 1 / 120] * 600)):
        results[name] = check_profile(mod, replacements, name, steps)
        print(f'{name}: maximum errors {results[name]}')
    results['state_changes'] = check_state_changes(mod, replacements)
    print(results['state_changes'])
    results['status'] = 'passed'
    report.write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('Additive loop checks passed; this does not establish the cause of issue #238.')


if __name__ == '__main__':
    main()
