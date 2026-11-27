"""Compile the Vulkan command resolver against a fake driver, without a GPU or game data."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DispatchTest(unittest.TestCase):
    def test_core_aliases_and_missing_entrypoints(self):
        cc = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        if not cc.is_file():
            self.skipTest('MSYS2 UCRT64 clang required')
        source = r'''
#include <assert.h>
#include <stdint.h>
#include "vk_dispatch.h"
static int use_ext, missing, call_index;
static const char *core[] = {"vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2",
    "vkCmdWriteTimestamp2", "vkCmdBindVertexBuffers2", "vkQueueSubmit2"};
static const char *ext[] = {"vkCmdBeginRenderingKHR", "vkCmdEndRenderingKHR", "vkCmdPipelineBarrier2KHR",
    "vkCmdWriteTimestamp2KHR", "vkCmdBindVertexBuffers2EXT", "vkQueueSubmit2KHR"};
static VKAPI_ATTR void VKAPI_CALL stub(void) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get(VkDevice d, const char *name) {
    assert(d == (VkDevice)(uintptr_t)42);
    int i = call_index++;
    assert(i < 6 && !strcmp(name, (use_ext ? ext : core)[i]));
    return i == missing ? NULL : stub;
}
int main(void) {
    VkApiDispatch table, zero = {0};
    for (use_ext = 0; use_ext < 2; use_ext++) {
        for (missing = -1; missing < 6; missing++) {
            memset(&table, 0x7f, sizeof table);
            call_index = 0;
            int ok = vk_api_load(&table, (VkDevice)(uintptr_t)42, get, use_ext);
            assert(call_index == 6);
            assert(ok == (missing == -1));
            if (!ok) assert(!memcmp(&table, &zero, sizeof table));
            else assert(table.QueueSubmit2 && table.BeginRendering && table.BindVertexBuffers2);
        }
    }
    assert(!vk_api_load(&table, VK_NULL_HANDLE, NULL, 0));
    assert(!memcmp(&table, &zero, sizeof table));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'resolver.c', Path(tmp) / 'resolver.exe'
            src.write_text(source)
            compiled = subprocess.run([str(cc), '-O2', '-I', str(ROOT / 'runtime'), str(src), '-o', str(exe)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
