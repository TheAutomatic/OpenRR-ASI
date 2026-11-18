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
        msgpack_path = self.out / 'gfx1201/openrr-d3d12-v1/manifest.msgpack'
        manifest_path = self.out / 'gfx1201/openrr-d3d12-v1/manifest.json'
        root = manifest_path.parent
        def fail_selected(args, **kwargs):
            if Path(args[1]).stem == 'selected':
                return subprocess.CompletedProcess(args, 1, '', 'simulated capture crash')
            return self.fake_capture(args, **kwargs)
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--precision', 'native', '--matrix-loads', 'vector8']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', fail_selected), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertFalse(manifest_path.exists())
        self.assertFalse(msgpack_path.exists())
        self.assertEqual(list(root.glob('*.nrp')) if root.exists() else [], [])
        failures = json.loads((self.raw / 'failures.json').read_text())
        self.assertEqual(len(failures['failures']), 1)
        self.assertIn('selected.spv', failures['failures'][0]['spv'])

    def test_failed_capture_preserves_consistent_previous_package(self):
        m = self.build()
        root = self.manifest_path.parent
        orig_manifest = self.manifest_path.read_bytes()
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()
        orig_nrps = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}
        def fail_selected_mutating_ordinary(args, **kwargs):
            if Path(args[1]).stem == 'selected':
                return subprocess.CompletedProcess(args, 1, '', 'simulated capture crash')
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
                '--precision', 'native', '--matrix-loads', 'vector8', '--refresh']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', fail_selected_mutating_ordinary), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertEqual(self.manifest_path.read_bytes(), orig_manifest)
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}, orig_nrps)
        for rec in m['records'].values():
            self.assertEqual(hashlib.sha256((root / rec['record']).read_bytes()).hexdigest(), rec['record_sha256'])
        self.assertTrue((self.raw / 'failures.json').is_file())

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

    def test_interrupt_between_backup_and_publish_rolls_back_previous_package(self):
        m = self.build()
        root = self.manifest_path.parent
        orig_manifest = self.manifest_path.read_bytes()
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()
        orig_nrps = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}
        real_replace = os.replace
        calls = []
        def faulty_replace(src, dst):
            calls.append((Path(src), Path(dst)))
            if len(calls) == 2:
                raise KeyboardInterrupt('simulated interrupt between backup and publish')
            return real_replace(src, dst)
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--precision', 'global', '--matrix-loads', 'vector8', '--refresh']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', self.fake_capture), \
             patch.object(producer.os, 'replace', faulty_replace), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(KeyboardInterrupt):
            producer.main()
        self.assertEqual(len(calls), 3)
        self.assertEqual(self.manifest_path.read_bytes(), orig_manifest)
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}, orig_nrps)
        for rec in m['records'].values():
            self.assertEqual(hashlib.sha256((root / rec['record']).read_bytes()).hexdigest(), rec['record_sha256'])
        self.assertEqual(list(root.parent.glob('.openrr-d3d12-v1.*')), [])

    def test_publish_and_rollback_double_failure_preserves_backup_and_recovers(self):
        m = self.build()
        root = self.manifest_path.parent
        backup_root = root.parent / f'.{producer.LAYOUT}.backup'
        orig_manifest = self.manifest_path.read_bytes()
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()
        orig_nrps = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.glob('*.nrp')}
        real_replace = os.replace
        calls = []
        def double_fault_replace(src, dst):
            calls.append((Path(src), Path(dst)))
            if len(calls) == 2:
                raise OSError('simulated publish failure')
            if len(calls) == 3:
                raise OSError('simulated rollback failure')
            return real_replace(src, dst)
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--precision', 'global', '--matrix-loads', 'vector8', '--refresh']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', self.fake_capture), \
             patch.object(producer.os, 'replace', double_fault_replace), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(OSError):
            producer.main()
        self.assertEqual(len(calls), 3)
        self.assertFalse(root.exists())
        self.assertTrue(backup_root.exists())
        self.assertEqual((backup_root / 'manifest.json').read_bytes(), orig_manifest)
        self.assertEqual((backup_root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertEqual({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in backup_root.glob('*.nrp')}, orig_nrps)
        for rec in m['records'].values():
            self.assertEqual(hashlib.sha256((backup_root / rec['record']).read_bytes()).hexdigest(), rec['record_sha256'])
        # Next run automatically recovers the backup package instead of wiping it.
        self.calls.clear()
        self.build()
        self.assertEqual(self.calls, [])
        self.assertTrue(root.exists())
        self.assertEqual(self.manifest_path.read_bytes(), orig_manifest)
        self.assertEqual(list(root.parent.glob('.openrr-d3d12-v1.*')), [])

    def test_mismatched_msgpack_preserves_backup_and_recovers(self):
        m = self.build()
        root = self.manifest_path.parent
        backup_root = root.parent / f'.{producer.LAYOUT}.backup'
        orig_manifest_bytes = self.manifest_path.read_bytes()
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()

        def failing_capture(args, **kwargs):
            return subprocess.CompletedProcess(args, 1, '', 'boom')

        fail_args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                     '--capture', str(self.capture), '--capture-dir', str(self.raw),
                     '--precision', 'global', '--matrix-loads', 'vector8', '--refresh']

        # Sub-case A: valid msgpack binding wrong manifest_hash
        shutil.copytree(root, backup_root)
        bad_hash_idx = producer._expected_index(m, orig_manifest_bytes)
        bad_hash_idx['manifest_hash'] ^= 1
        (root / 'manifest.msgpack').write_bytes(producer.msgpack(bad_hash_idx))
        with patch.object(sys, 'argv', fail_args), patch.object(producer.subprocess, 'run', failing_capture), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertFalse(backup_root.exists())
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertTrue(producer._is_consistent_package(root))

        # Sub-case B: valid msgpack keeping real manifest_hash but mismatched record metadata
        shutil.copytree(root, backup_root)
        bad_rec_idx = producer._expected_index(m, orig_manifest_bytes)
        first_key = next(iter(bad_rec_idx['records']))
        bad_rec_idx['records'][first_key]['code_hash'] = '0000000000000000'
        (root / 'manifest.msgpack').write_bytes(producer.msgpack(bad_rec_idx))
        with patch.object(sys, 'argv', fail_args), patch.object(producer.subprocess, 'run', failing_capture), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertFalse(backup_root.exists())
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertTrue(producer._is_consistent_package(root))

    def test_signed_int_msgpack_tag_triggers_backup_restore(self):
        m = self.build()
        root = self.manifest_path.parent
        backup_root = root.parent / f'.{producer.LAYOUT}.backup'
        orig_msgpack = (root / 'manifest.msgpack').read_bytes()
        # Replace uint64 schema=1 (\xcf...1) with int64 schema=1 (\xd3...1). Python decodes
        # both as 1, while C++ pal::Value::number() requires unsigned UInt tags.
        uint_schema = producer.msgpack('schema') + producer.msgpack(1)
        int_schema = producer.msgpack('schema') + b'\xd3\x00\x00\x00\x00\x00\x00\x00\x01'
        self.assertIn(uint_schema, orig_msgpack)
        corrupted = orig_msgpack.replace(uint_schema, int_schema, 1)
        self.assertEqual(len(corrupted), len(orig_msgpack))
        self.assertEqual(producer._unpack_msgpack(corrupted), producer._unpack_msgpack(orig_msgpack))

        shutil.copytree(root, backup_root)
        (root / 'manifest.msgpack').write_bytes(corrupted)
        self.assertFalse(producer._is_consistent_package(root))

        def failing_capture(args, **kwargs):
            return subprocess.CompletedProcess(args, 1, '', 'boom')

        fail_args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                     '--capture', str(self.capture), '--capture-dir', str(self.raw),
                     '--precision', 'global', '--matrix-loads', 'vector8', '--refresh']
        with patch.object(sys, 'argv', fail_args), patch.object(producer.subprocess, 'run', failing_capture), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            producer.main()
        self.assertFalse(backup_root.exists())
        self.assertEqual((root / 'manifest.msgpack').read_bytes(), orig_msgpack)
        self.assertTrue(producer._is_consistent_package(root))

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

    def test_gfx1200_target_and_mach_consistency(self):
        cfg = json.loads(self.cfg.read_text())
        cfg['patch_version'] = 5
        self.cfg.write_text(json.dumps(cfg))
        seen_ids = []
        def cap_target(args, **kwargs):
            seen_ids.append(kwargs['env']['AMDGPU_GPU_ID'])
            return self.fake_capture(args, **kwargs)
        args = ['records', str(self.inputs), '--out', str(self.out), '--toolchain', str(self.cfg),
                '--capture', str(self.capture), '--capture-dir', str(self.raw),
                '--target', 'gfx1200', '--precision', 'global', '--matrix-loads', 'vector8']
        with patch.object(sys, 'argv', args), patch.object(producer.subprocess, 'run', cap_target), \
             contextlib.redirect_stdout(io.StringIO()):
            producer.main()
        self.assertEqual(seen_ids, ['gfx1200'] * 4)
        pkg = self.out / 'gfx1200' / producer.LAYOUT
        m = json.loads((pkg / 'manifest.json').read_text())
        self.assertEqual(m['target'], 'gfx1200')
        self.assertEqual(m['patch_version'], 5)
        self.assertTrue(producer._is_consistent_package(pkg, 'gfx1200'))
        for rec in m['records'].values():
            raw = (pkg / rec['record']).read_bytes()
            self.assertEqual(struct.unpack_from('<I', raw, 76)[0], 0x48)
        # Swap a 0x4e (gfx1201) .nrp header into gfx1200 package without updating sha256/mach -> rejected
        first = next(iter(m['records'].values()))
        nrp_path = pkg / first['record']
        orig_nrp = nrp_path.read_bytes()
        bad_nrp = orig_nrp[:76] + struct.pack('<I', 0x4e) + orig_nrp[80:]
        nrp_path.write_bytes(bad_nrp)
        m['records'][first['spv_hash']]['record_sha256'] = hashlib.sha256(bad_nrp).hexdigest()
        mb = (json.dumps(m, indent=2) + '\n').encode('utf-8')
        (pkg / 'manifest.json').write_bytes(mb)
        (pkg / 'manifest.msgpack').write_bytes(producer.msgpack(producer._expected_index(m, mb)))
        self.assertFalse(producer._is_consistent_package(pkg, 'gfx1200'))

    def test_runtime_declared_fastmode_stems_match_build(self):
        root = Path(__file__).parents[2]
        rt = (root / 'runtime/src/runtime.cpp').read_text(encoding='utf-8')
        import re
        def grab(fn):
            m = re.search(rf'{fn}\(\)\s*\{{.*?v\s*=\s*\{{(.*?)\}};', rt, re.S)
            self.assertIsNotNone(m, f'missing {fn}')
            return set(re.findall(r'"([^"]+)"', m.group(1)))
        lt_core = grab('declared_core_lt_stems')
        lt_wide = grab('declared_wide_lt_stems')
        lt_direct = grab('declared_direct_lt_stems')
        lx_stems = grab('declared_lx_stems')
        flow_match = re.search(r'Engine::flow_kernel_names\(\)\s*\{\s*return\s*\{(.*?)\};',
                               (root / 'gpu/src/engine.cpp').read_text(encoding='utf-8'), re.S)
        self.assertIsNotNone(flow_match)
        lt_flow = set(re.findall(r'"([^"]+)"', flow_match.group(1)))
        all_lt = lt_core | lt_wide | lt_direct | lt_flow
        self.assertEqual(len(all_lt), 40, f'expected 40 _lt stems, got {len(all_lt)}')
        self.assertEqual(len(lx_stems), 4, f'expected 4 _lx stems, got {len(lx_stems)}')
        sh = root / 'build/v017-asi/stage/openrr-amd/shaders'
        if sh.is_dir():
            for s in all_lt:
                self.assertTrue((sh / f'{s}_lt.spv').is_file(), f'missing {s}_lt.spv')
            for s in lx_stems:
                self.assertTrue((sh / f'{s}_lx.spv').is_file(), f'missing {s}_lx.spv')

if __name__ == '__main__':
    unittest.main()
