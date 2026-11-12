"""DM_VK_UBO_TRIM: the bytes of a constant buffer a shader reads, from its SPIR-V (gfx_vk_exec.c spv_cb_used)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class UboTrim(unittest.TestCase):
    def test_scan(self):
        source = (ROOT / 'nxnative/dm/android/gfx_vk_exec.c').read_text()
        fn = source[source.index('static uint32_t spv_cb_used('):source.index('static uint32_t cb_used(')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
''' + fn + r'''
// a module: %3 has Binding 14, %4 Binding 15; constants %10 = 0, %11 = 5, %12 = 40; %20 a dynamic index
#define I(op, len) ((uint32_t)(len) << 16 | (op))
static uint32_t m[64]; static uint32_t n;
static void head(void) { n = 0; m[n++] = 0x07230203; m[n++] = 0x10000; m[n++] = 0; m[n++] = 32; m[n++] = 0;
    m[n++] = I(71, 4); m[n++] = 3; m[n++] = 33; m[n++] = 14; m[n++] = I(71, 4); m[n++] = 4; m[n++] = 33; m[n++] = 15;
    m[n++] = I(43, 4); m[n++] = 1; m[n++] = 10; m[n++] = 0; m[n++] = I(43, 4); m[n++] = 1; m[n++] = 11; m[n++] = 5; m[n++] = I(43, 4); m[n++] = 1; m[n++] = 12; m[n++] = 40;
    m[n++] = I(59, 4); m[n++] = 2; m[n++] = 3; m[n++] = 2; m[n++] = I(59, 4); m[n++] = 2; m[n++] = 4; m[n++] = 2; }
static uint32_t rid = 21;
static void chain(uint32_t var, uint32_t idx) { m[n++] = I(65, 6); m[n++] = 5; m[n++] = rid++; m[n++] = var; m[n++] = 10; m[n++] = idx; }
int main(void) {
    head(); chain(3, 11); chain(3, 12); chain(4, 11);
    assert(spv_cb_used(m, n, 14) == 41 * 16);   // the highest constant index, 40
    assert(spv_cb_used(m, n, 15) == 6 * 16);
    assert(spv_cb_used(m, n, 16) == 65536);     // no such binding: the whole range
    chain(4, 20);                                // a dynamic index
    assert(spv_cb_used(m, n, 15) == 65536 && spv_cb_used(m, n, 14) == 41 * 16);
    m[n++] = I(61, 4); m[n++] = 6; m[n++] = 30; m[n++] = 3;   // OpLoad of the whole block
    assert(spv_cb_used(m, n, 14) == 65536);
    head(); assert(spv_cb_used(m, n, 14) == 16);   // never read: one vec4
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_ubo_trim_') as tmp:
            c = Path(tmp) / 't.c'; exe = Path(tmp) / 't'; c.write_text(code)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-o', str(exe), str(c)], check=True)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
