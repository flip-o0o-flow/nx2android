"""The device-Oodle route for Unreal Engine 3 shader caches (Batman: Arkham Knight), without a device: the job pipeline.device_oodle_ue3 writes, the decoded file it makes
from the device's answer, and shader_split.ue3_body reading that file in place of the Mac host's decoder."""
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
os.environ.setdefault('DM_GAME', '010015100B514000')
sys.path.insert(0, str(REPO / 'nxnative/dm/gfx/tools'))
from porter import pipeline
import shader_split

TAG = bytes.fromhex('c1832a9e')
FAKE = bytes([0x8c, 0x0a])   # an "Oodle" block: 8C 0A + the raw bytes reversed (the fake decoder below reverses them back)


def package(blocks):
    """A UE3 package: a summary, then one chunk whose blocks are given as (kind, raw bytes): stored, zlib or oodle"""
    stored = [b if k == 'stored' else zlib.compress(b) if k == 'zlib' else FAKE + b[::-1] for k, b in blocks]
    raw = b''.join(b for _, b in blocks)
    summary = TAG + struct.pack('<H', 864) + bytes(10)
    chunk = TAG + struct.pack('<III', 0x40, sum(map(len, stored)), len(raw)) + b''.join(struct.pack('<II', len(s), len(b)) for s, (_, b) in zip(stored, blocks)) + b''.join(stored)
    return summary + chunk, summary + raw


class OodleUE3(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(prefix='nx2a-oodle-ue3-'); self.addCleanup(t.cleanup); self.root = Path(t.name)
        rom = self.root / 'v0/RomFS/BmGame/CookedSwitch'; rom.mkdir(parents=True)
        self.blocks = [('oodle', bytes(range(0x40))), ('stored', b'S' * 0x40), ('zlib', b'Z' * 0x40), ('oodle', b'last block')]
        self.pkg, self.body = package(self.blocks)
        (rom / 'CombinedShaderCache-Switch.xxx').write_bytes(self.pkg)
        (rom / 'GlobalShaderCache-Switch.bin').write_bytes(b'BMSG' + bytes(12))
        (rom / 'Other.xxx').write_bytes(package([('oodle', b'x' * 0x40)])[0])   # not a shader cache: not in the job
        p = self.port = object.__new__(pipeline.Port)
        p.work, p.versions, p.dirs, p.app_name = self.root / 'work', ['v0'], {'v0': self.root / 'v0'}, 'test'
        p.oodle_why = '2 Unreal Engine 3 package(s) with Oodle-compressed chunks'
        self.jobs = []
        def decode(job, out, bnsh=False):   # the device: DMOJ records (u32 compressed, u32 raw, bytes) → DMOR (u32 length, bytes)
            self.assertFalse(bnsh)
            d = job.read_bytes(); n = struct.unpack_from('<I', d, 4)[0]; o, res = 8, [b'DMOR' + struct.pack('<I', n)]
            for _ in range(n):
                c, u = struct.unpack_from('<II', d, o); blk = d[o + 8:o + 8 + c]; o += 8 + c
                self.assertEqual(blk[:2], FAKE); res.append(struct.pack('<I', u) + blk[2:][::-1])
            self.jobs.append(n); out.write_bytes(b''.join(res)); job.unlink()
        p.device_oodle_job = decode

    def test_shader_cache_blocks_round_trip(self):
        env = self.port.device_oodle()
        self.assertEqual(self.jobs, [2])   # the two Oodle blocks of the shader cache only
        os.environ['DM_GFPAK_DECODED'] = env['DM_GFPAK_DECODED']; self.addCleanup(os.environ.pop, 'DM_GFPAK_DECODED')
        label = '/BmGame/CookedSwitch/CombinedShaderCache-Switch.xxx'
        self.assertEqual(len(shader_split.ue3_decoded(label, len(self.pkg))), 2)
        self.assertIsNone(shader_split.ue3_decoded(label, len(self.pkg) + 1))   # another size: not this package
        self.assertEqual(shader_split.ue3_body(self.pkg, label), self.body)
        self.assertIsNone(shader_split.ue3_body(package([('oodle', b'x' * 0x40)])[0], '/BmGame/CookedSwitch/Other.xxx'))   # not decoded on the device: skipped

    def test_other_routes_untouched(self):
        self.port.oodle_why = '3 Game Freak GFPAK archive(s) with Oodle Kraken entries'
        called = []
        self.port.device_oodle_ue3 = lambda: called.append(1)
        with self.assertRaises(Exception): self.port.device_oodle()   # the GFPAK path runs (and finds no tools here), not the UE3 one
        self.assertEqual(called, [])


if __name__ == '__main__':
    unittest.main()
