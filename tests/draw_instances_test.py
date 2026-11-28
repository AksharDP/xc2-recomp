"""Compile the real NVN front end and check empty and positive instanced draws.

The unused host callbacks abort if reached. No game data, renderer or window is needed.
NVN_CMD_TEST_SOURCE may select an unchanged nvn_api_cmd.c for a before/after reproduction.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "nvn_api.h"
#include "platform/platform.h"
unsigned rt_trace;
GfxStats gfx_counters;
void nvn_called(uint8_t *seen, const char *name) { (void)seen; (void)name; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
void rt_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void nvnp_nvnCommandBufferDrawArraysInstanced(Cpu *c);
void nvnp_nvnCommandBufferDrawElementsInstanced(Cpu *c);
static int one(int indexed, int instances, int seeded) {
    Cpu c = {0};
    cmd_t cmd = {0};
    uint16_t indices[6] = {0,1,2,0,2,3};
    c.x[0] = (uintptr_t)&cmd;
    c.x[1] = NVN_DRAW_PRIMITIVE_TRIANGLES;
    if (seeded) {
        c.x[3] = 3; c.x[5] = 1;
        nvnp_nvnCommandBufferDrawArraysInstanced(&c);
    }
    size_t before = cmd.cmd_used, hw_before = cmd.hw_used;
    uint8_t prefix[64];
    if (before > sizeof prefix) abort();
    if (before) memcpy(prefix, cmd.seg, before);
    if (indexed) {
        c.x[2] = NVN_INDEX_TYPE_UNSIGNED_SHORT; c.x[3] = 6; c.x[4] = (uintptr_t)indices;
        c.x[5] = 5; c.x[6] = 7; c.x[7] = (uint64_t)(int64_t)instances;
        nvnp_nvnCommandBufferDrawElementsInstanced(&c);
    } else {
        c.x[2] = 0; c.x[3] = 6; c.x[4] = 7; c.x[5] = (uint64_t)(int64_t)instances;
        nvnp_nvnCommandBufferDrawArraysInstanced(&c);
    }
    int expected_record = instances > 0;
    int actual_record = cmd.cmd_used != before;
    printf("%s instances=%d: recorded=%d expected=%d hw=%zu\n", indexed ? "elements" : "arrays", instances,
           actual_record, expected_record, cmd.hw_used);
    if (expected_record && cmd.seg) {
        const op_hdr *h = (const op_hdr *)(cmd.seg + before);
        if (h->op != (indexed ? OP_DRAW_IDX : OP_DRAW)) actual_record = 0;
        if (indexed) {
            const p_draw_idx *p = nvn_op_payload(h);
            if (p->instances != instances || p->base_instance != 7 || p->base_vertex != 5 ||
                p->count != 6 || p->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT || p->ibuf != (uintptr_t)indices)
                actual_record = 0;
        } else {
            const p_draw *p = nvn_op_payload(h);
            if (p->instances != instances || p->base_instance != 7 || p->first != 0 || p->count != 6)
                actual_record = 0;
        }
    }
    int bad = actual_record != expected_record || (!expected_record && cmd.hw_used != hw_before) ||
              (before && memcmp(prefix, cmd.seg, before));
    free(cmd.seg);
    return bad;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    return one(!strcmp(argv[1], "elements"), (int)strtol(argv[2], NULL, 10), atoi(argv[3]));
}

void plat_mutex_lock(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; abort(); }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; abort(); }
uint64_t gfx_replayed_submits(void) { abort(); }
int cfg_is_set(const char *name) { (void)name; abort(); }
int cfg_bool(const char *name) { (void)name; abort(); }
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c; (void)fn; (void)n; abort(); }
'''


class InstancedDrawTest(unittest.TestCase):
    def test_empty_draws_do_not_record_and_positive_arguments_survive(self):
        default_cc = r'C:\msys64\ucrt64\bin\clang.exe' if os.name == 'nt' else 'clang'
        cc = shutil.which(os.environ.get('CC', default_cc))
        if not cc:
            self.skipTest('Clang required')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'instances.c', Path(tmp) / 'instances.exe'
            src.write_text(SOURCE, encoding='utf-8')
            commands = os.environ.get('NVN_CMD_TEST_SOURCE', str(ROOT / 'runtime/nvn_api_cmd.c'))
            built = subprocess.run([cc, '-O2', '-ffunction-sections', '-fdata-sections', '-fuse-ld=lld',
                                    '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), str(src), commands,
                                    '-o', str(exe), *([] if os.name == 'nt' else ['-lm'])],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for name in ('arrays', 'elements'):
                for count in (0, -1, 1, 3):
                    for seeded in (False, True):
                        with self.subTest(draw=name, instances=count, existing_command=seeded):
                            run = subprocess.run([str(exe), name, str(count), str(int(seeded))],
                                                 capture_output=True, text=True, timeout=10)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
