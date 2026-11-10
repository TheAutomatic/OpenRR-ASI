# SPDX-License-Identifier: MIT
"""CPU-only tests of opt-in record policy, fingerprint and cache integrity."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('producer', Path(__file__).parents[1]/'build/build_aco_records.py')
producer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(producer)

class BuildPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inputs = self.root/'shaders'
        self.inputs.mkdir()
        for i, name in enumerate(('selected', 'ordinary')):
            words = [0x07230203, 0x00010300, 0, 10+i, 0, (6<<16)|16, 1, 17, 32, 1, 1]
            (self.inputs/(name+'.spv')).write_bytes(struct.pack('<11I', *words))
        self.capture = self.root/'capture-tool'
        self.capture.write_bytes(b'cpu-test')
        self.cfg = self.root/'toolchain.json'
        self.cfg.write_text(json.dumps(dict(icd='unused', shim='unused', library_path='unused',
            mesa_commit='pinned', patch_version=4, cmat_address_patch_sha256='patch-a')))
        self.out = self.root/'records'
        self.raw = self.root/'raw'
        self.calls = []

    def fake_capture(self, args, **kwargs):
        env = kwargs['env']
        self.calls.append((Path(args[1]).stem, env.get('ONR_ACO_CMAT_ADDRESS')))
        prefix = Path(env['ONR_ACO_CAPTURE'])
        info = dict(inline=[], rsrc1=0, rsrc2=0, rsrc3=0, exec_bytes=4, workgroup_size=32,
            shared_bytes=0, push_bytes=0, user_sgprs=0, set0=-1, push=-1, grid=-1,
            scratch_bytes=0, wave_size=32, vgprs=8)
        prefix.with_suffix('.json').write_text(json.dumps(info))
        prefix.with_suffix('.code').write_bytes(struct.pack('<I', 0xbfb00000))
        prefix.with_suffix('.layouts').write_bytes(b'layout')
        return subprocess.CompletedProcess(args, 0, 'ONR_M0 subst=0 writes=0 constdata=0 pc_relative=0\n', '')

    def build(self, selected=('selected',)):
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
            '--capture', str(self.capture), '--capture-dir', str(self.raw),
            '--precision', 'global', '--matrix-loads', 'vector8', '--cmat-address-shaders', *selected]
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', self.fake_capture), \
             patch.dict(os.environ, {'ONR_ACO_CMAT_ADDRESS':'1'}), contextlib.redirect_stdout(io.StringIO()):
            producer.main()
        self.manifest_path = self.out/'gfx1201/openrr-d3d12-v1/manifest.json'
        return json.loads(self.manifest_path.read_text())

    def test_exact_selection_and_cache(self):
        m = self.build()
        self.assertEqual(sorted(self.calls), [('ordinary',None)]*2+[('selected','1')]*2)
        self.assertEqual(m['compile_options']['cmat_address_shaders'], ['selected'])
        self.calls.clear()
        self.build()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.manifest_path.parent/'capture').exists())

    def test_policy_and_toolchain_invalidate(self):
        a = self.build()
        self.calls.clear()
        b = self.build(())
        self.assertNotEqual(a['fingerprint'], b['fingerprint'])
        self.assertEqual(len(self.calls), 4)
        self.assertTrue(all(flag is None for _, flag in self.calls))
        cfg = json.loads(self.cfg.read_text())
        cfg['cmat_address_patch_sha256'] = 'patch-b'
        self.cfg.write_text(json.dumps(cfg))
        c = self.build(())
        self.assertNotEqual(b['fingerprint'], c['fingerprint'])

    def test_corrupt_record_recaptured(self):
        m = self.build()
        item = next(iter(m['records'].values()))
        (self.manifest_path.parent/item['record']).write_bytes(b'broken')
        self.calls.clear()
        self.build()
        self.assertEqual(len(self.calls), 2)

    def test_missing_selection_or_patch_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build(('absent',))
        cfg = json.loads(self.cfg.read_text())
        del cfg['cmat_address_patch_sha256']
        self.cfg.write_text(json.dumps(cfg))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build()

    def test_identical_spirv_conflicting_policy_rejected(self):
        (self.inputs/'ordinary.spv').write_bytes((self.inputs/'selected.spv').read_bytes())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build()

if __name__ == '__main__':
    unittest.main()
