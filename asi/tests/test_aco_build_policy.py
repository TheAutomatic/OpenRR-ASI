# SPDX-License-Identifier: MIT
"""CPU-only tests of opt-in record policy, fingerprint and cache integrity."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
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
        self.calls.append((Path(args[1]).stem, env.get('ONR_ACO_CMAT_ADDRESS'),
                           env.get('ONR_ACO_CU_MODE'), env.get('ONR_ACO_WGP_MODE'),
                           env.get('ONR_ACO_SAMPLE_WAIT_ZERO'), env.get('ONR_ACO_SHARED_CMAT')))
        prefix = Path(env['ONR_ACO_CAPTURE'])
        info = dict(inline=[], rsrc1=0, rsrc2=0, rsrc3=0, exec_bytes=4, workgroup_size=32,
            shared_bytes=0, push_bytes=0, user_sgprs=0, set0=-1, push=-1, grid=-1,
            scratch_bytes=0, wave_size=32, vgprs=8)
        prefix.with_suffix('.json').write_text(json.dumps(info))
        prefix.with_suffix('.code').write_bytes(struct.pack('<I', 0xbfb00000))
        prefix.with_suffix('.layouts').write_bytes(b'layout')
        return subprocess.CompletedProcess(args, 0, 'ONR_M0 subst=0 writes=0 constdata=0 pc_relative=0\n', '')

    def build(self, selected=('selected',), cu=(), wgp=(), swz=(), shared_cmat_shaders=(), shared_cmat_mode='load'):
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
            '--capture', str(self.capture), '--capture-dir', str(self.raw),
            '--precision', 'global', '--matrix-loads', 'vector8', '--cmat-address-shaders', *selected,
            '--cu-mode-shaders', *cu, '--wgp-mode-shaders', *wgp, '--sample-wait-zero-shaders', *swz,
            '--shared-cmat', shared_cmat_mode if (shared_cmat_shaders or shared_cmat_mode != 'load') else 'off']
        if shared_cmat_shaders:
            args += ['--shared-cmat-shaders', *shared_cmat_shaders]
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', self.fake_capture), \
             patch.dict(os.environ, {'ONR_ACO_CMAT_ADDRESS': '1', 'ONR_ACO_CU_MODE': '1',
                                     'ONR_ACO_WGP_MODE': '1', 'ONR_ACO_SAMPLE_WAIT_ZERO': '1'}), \
             contextlib.redirect_stdout(io.StringIO()):
            producer.main()
        self.manifest_path = self.out/'gfx1201/openrr-d3d12-v1/manifest.json'
        return json.loads(self.manifest_path.read_text())

    def test_exact_selection_and_cache(self):
        m = self.build()
        self.assertEqual(sorted(self.calls), [('ordinary',None,None,None,None,None)]*2+[('selected','1',None,None,None,None)]*2)
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
        self.assertTrue(all(t[1:] == (None,)*5 for t in self.calls))
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
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build((), cu=('selected',))

    def test_cu_wgp_and_sample_wait_selection_and_isolation(self):
        m = self.build((), cu=('selected',), wgp=('ordinary',), swz=('ordinary',))
        self.assertEqual(sorted(self.calls),
                         [('ordinary', None, None, '1', '1', None)] * 2 + [('selected', None, '1', None, None, None)] * 2)
        self.assertEqual(m['compile_options']['cu_mode_shaders'], ['selected'])
        self.assertEqual(m['compile_options']['wgp_mode_shaders'], ['ordinary'])
        self.assertEqual(m['compile_options']['sample_wait_zero_shaders'], ['ordinary'])
        self.calls.clear()
        self.build((), cu=('selected',), wgp=('ordinary',), swz=('ordinary',))
        self.assertEqual(self.calls, [])
        n = self.build((), (), (), ())
        self.assertEqual(len(self.calls), 4)
        for k in ('cu_mode_shaders', 'wgp_mode_shaders', 'sample_wait_zero_shaders'):
            self.assertNotIn(k, n['compile_options'])

    def test_shared_cmat_shader_selection_and_isolation(self):
        # per-shader scope: the env var reaches only the listed stem, the manifest records
        # the scope (fingerprint isolation), and a bare global mode keeps the old behavior
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build((), shared_cmat_shaders=('selected',), shared_cmat_mode='off')   # mode off -> rejected
        m = self.build((), shared_cmat_shaders=('selected',))
        self.assertEqual(sorted(self.calls),
                         [('ordinary', None, None, None, None, None)] * 2
                          + [('selected', None, None, None, None, 'load')] * 2)
        self.assertEqual(m['compile_options']['shared_cmat'], 'load')
        self.assertEqual(m['compile_options']['shared_cmat_shaders'], ['selected'])
        rec = {r['spv']: r for r in m['records'].values()}
        self.assertEqual(rec['selected.spv']['shared_cmat'], 'load')
        self.assertEqual(rec['ordinary.spv']['shared_cmat'], 'off')
        self.calls.clear()
        self.build((), shared_cmat_shaders=('selected',))
        self.assertEqual(self.calls, [])   # same fingerprint: full cache hit
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build((), shared_cmat_shaders=('absent',))
        g = self.build((), shared_cmat_mode='loadstore')
        self.assertEqual(sorted(self.calls),
                         [('ordinary', None, None, None, None, 'loadstore')] * 2
                          + [('selected', None, None, None, None, 'loadstore')] * 2)
        self.assertNotIn('shared_cmat_shaders', g['compile_options'])

    def test_shared_cmat_env_not_leaked_and_policy_conflict_rejected(self):
        # a parent-env ONR_ACO_SHARED_CMAT must never reach any capture without the flag
        with patch.dict(os.environ, {'ONR_ACO_SHARED_CMAT': 'loadstore'}):
            self.build(())
        self.assertTrue(all(t[5] is None for t in self.calls))
        # identical SPIR-V bytes may not request differing per-stem policies (incl. shared-cmat)
        (self.inputs/'ordinary.spv').write_bytes((self.inputs/'selected.spv').read_bytes())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build(('selected',))                                  # cmat-address on one stem only
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build(('selected',), shared_cmat_shaders=('selected',))   # shared-cmat on one stem only

    def test_cu_wgp_overlap_or_missing_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.build((), cu=('selected',), wgp=('selected',))
        for kw in (dict(cu=('absent',)), dict(wgp=('absent',)), dict(swz=('absent',))):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.build((), **kw)

    def test_stale_nrp_pruned_on_success(self):
        m = self.build()
        orphan = self.manifest_path.parent / 'deadbeef01234567.nrp'
        orphan.write_bytes(b'stale')
        self.calls.clear()
        self.build()
        self.assertFalse(orphan.exists())
        self.assertEqual(sorted(p.name for p in self.manifest_path.parent.glob('*.nrp')),
                         sorted(r['record'] for r in m['records'].values()))

    def test_failed_capture_does_not_publish_partial_manifest(self):
        self.build()
        msgpack_path = self.manifest_path.parent / 'manifest.msgpack'
        self.assertTrue(self.manifest_path.exists() and msgpack_path.exists())
        def fail_selected(args, **kwargs):
            if Path(args[1]).stem == 'selected':
                return subprocess.CompletedProcess(args, 1, '', 'simulated capture crash')
            return self.fake_capture(args, **kwargs)
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--precision', 'native', '--matrix-loads', 'vector8', '--refresh']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', fail_selected), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertFalse(self.manifest_path.exists())
        self.assertFalse(msgpack_path.exists())
        failures = json.loads((self.raw / 'failures.json').read_text())
        self.assertEqual(len(failures['failures']), 1)
        self.assertIn('selected.spv', failures['failures'][0]['spv'])

    def test_interrupted_capture_preserves_consistent_previous_package(self):
        m = self.build()
        root = self.manifest_path.parent
        orig_manifest = self.manifest_path.read_bytes()
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()
        orig_nrps = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}
        def interrupt_on_selected(args, **kwargs):
            if Path(args[1]).stem == 'selected':
                raise KeyboardInterrupt()
            # Mutate returned code for ordinary.spv so an un-staged write would diverge from manifest.
            env = kwargs['env']
            prefix = Path(env['ONR_ACO_CAPTURE'])
            info = dict(inline=[], rsrc1=0, rsrc2=0, rsrc3=0, exec_bytes=4, workgroup_size=32,
                shared_bytes=0, push_bytes=0, user_sgprs=0, set0=-1, push=-1, grid=-1,
                scratch_bytes=0, wave_size=32, vgprs=8)
            prefix.with_suffix('.json').write_text(json.dumps(info))
            prefix.with_suffix('.code').write_bytes(struct.pack('<I', 0xbf800000))
            prefix.with_suffix('.layouts').write_bytes(b'layout')
            return subprocess.CompletedProcess(args, 0, 'ONR_M0 subst=0 writes=0 constdata=0 pc_relative=0\n', '')
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--precision', 'global', '--matrix-loads', 'vector8', '--cmat-address-shaders', 'selected', '--refresh']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', interrupt_on_selected), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(KeyboardInterrupt):
            producer.main()
        self.assertEqual(self.manifest_path.read_bytes(), orig_manifest)
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}, orig_nrps)
        for rec in m['records'].values():
            self.assertEqual(hashlib.sha256((root / rec['record']).read_bytes()).hexdigest(), rec['record_sha256'])
        self.assertEqual(list(root.parent.glob('.openrr-d3d12-v1.*')), [])

    def test_gfx11_shader_strip_policy(self):
        bs_spec = importlib.util.spec_from_file_location('build_shaders', Path(__file__).parents[1]/'build/build_shaders.py')
        bs_mod = importlib.util.module_from_spec(bs_spec)
        bs_spec.loader.exec_module(bs_mod)
        src_text = (Path(__file__).parents[1]/'build/build_shaders.py').read_text()
        self.assertIn('"W2_96UP=1"', src_text)
        self.assertIn('"LT_PV8=1"', src_text)
        glslang = shutil.which('glslangValidator') or shutil.which('glslang')
        if not glslang:
            for cand in (r'C:\VulkanSDK\1.4.304.0\Bin\glslangValidator.exe', r'C:\VulkanSDK\1.3.296.0\Bin\glslangValidator.exe'):
                if os.path.exists(cand):
                    glslang = cand
                    break
        if glslang:
            gpu_dir = Path(__file__).parents[2]/'gpu'
            out_g12 = self.root/'spv_g12'
            out_g11 = self.root/'spv_g11'
            subprocess.run([sys.executable, str(Path(__file__).parents[1]/'build/build_shaders.py'), glslang, str(gpu_dir), str(out_g12)], check=True, stdout=subprocess.DEVNULL)
            subprocess.run([sys.executable, str(Path(__file__).parents[1]/'build/build_shaders.py'), glslang, str(gpu_dir), str(out_g11), '--gfx11'], check=True, stdout=subprocess.DEVNULL)
            for suffix in ('f_lt', 'fl_lt'):
                self.assertEqual(producer.dimensions((out_g12/f'dec_up_96_128_128_{suffix}.spv').read_bytes()), (64, 1, 1))
                self.assertEqual(producer.dimensions((out_g11/f'dec_up_96_128_128_{suffix}.spv').read_bytes()), (128, 1, 1))

if __name__ == '__main__':
    unittest.main()
