#!/usr/bin/env python3
"""Capture OpenRR SPIR-V with the pinned RADV and pack NRPAL002 records."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

LAYOUT = 'openrr-d3d12-v1'
TARGETS = {'gfx1201': 0x4e, 'gfx1200': 0x48}

def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h

def dimensions(data):
    w = struct.unpack('<' + 'I' * (len(data)//4), data)
    if w[0] != 0x07230203:
        raise ValueError('invalid SPIR-V')
    constants, dims, ids = {}, None, None
    p = 5
    while p < len(w):
        n, op = w[p] >> 16, w[p] & 65535
        if n == 0 or p+n > len(w):
            raise ValueError('invalid SPIR-V instruction')
        a = w[p+1:p+n]
        if op == 16 and a[1] == 17:
            dims = a[2:5]
        elif op == 331 and a[1] == 38:
            ids = a[2:5]
        elif op == 43:
            constants[a[1]] = a[2]
        p += n
    if ids:
        dims = tuple(constants[x] for x in ids)
    if not dims or len(dims) != 3 or not all(dims):
        raise ValueError('no literal workgroup dimensions')
    return dims

def adapt_code(code, other, exec_size, info, other_info, logs):
    """Use ACO's operand-level substitution; no disassembly-based rewriting."""
    if exec_size <= 0 or exec_size > len(code) or exec_size % 4 or len(code) % 4:
        raise ValueError('invalid code sizes')
    if any(x[0] != 0xbf9f0000 for x in struct.iter_unpack('<I', code[exec_size:])):
        raise ValueError('constant data after executable code is unsupported')
    if code != other or info != other_info:
        raise ValueError('address32 sentinel captures differ in code or resource/argument metadata')
    reports = []
    for log in logs:
        lines = re.findall(r'^ONR_M0 (.+)$', log, re.MULTILINE)
        if len(lines) != 1:
            raise ValueError('expected exactly one source-level ACO address report')
        report = {k: int(v) for k, v in (x.split('=') for x in lines[0].split())}
        if set(report) != {'subst', 'writes', 'constdata', 'pc_relative'}:
            raise ValueError('invalid ACO address report')
        if report['writes'] or report['constdata'] or report['pc_relative']:
            raise ValueError(f'unsupported body state: {report}')
        reports.append(report)
    if reports[0] != reports[1]:
        raise ValueError('address32 substitution reports differ')
    return code, reports[0]

def pack(info, code, spv_hash, mach=0x4e):
    pairs = info['inline']
    fields = [info[k] for k in ('rsrc1', 'rsrc2', 'rsrc3', 'exec_bytes', 'workgroup_size',
                                'shared_bytes', 'push_bytes', 'user_sgprs')]
    header = struct.pack('<8sQQ8I3i4I', b'NRPAL002', spv_hash, fnv(code), *fields,
                         info['set0'], info['push'], info['grid'], len(pairs), len(code),
                         mach, info['scratch_bytes'])
    assert len(header) == 84
    return header + b'\0'*12 + b''.join(struct.pack('<ii', *p) for p in pairs) + code

def msgpack(value):
    if isinstance(value, int):
        return b'\xcf' + struct.pack('>Q', value)
    if isinstance(value, str):
        data = value.encode('utf-8')
        return b'\xdb' + struct.pack('>I', len(data)) + data
    if isinstance(value, list):
        return b'\xdd' + struct.pack('>I', len(value)) + b''.join(map(msgpack, value))
    if isinstance(value, dict):
        return b'\xdf' + struct.pack('>I', len(value)) + b''.join(msgpack(k)+msgpack(v) for k,v in value.items())
    raise TypeError(type(value))

def _unpack_msgpack(data: bytes):
    mv = memoryview(data)
    def read(pos: int, n: int):
        if pos + n > len(mv):
            raise ValueError('truncated msgpack')
        return bytes(mv[pos:pos + n]), pos + n
    def decode(pos: int):
        if pos >= len(mv):
            raise ValueError('unexpected EOF in msgpack')
        b = mv[pos]
        pos += 1
        if b <= 0x7f:
            return b, pos
        if b >= 0xe0:
            return b - 256, pos
        if 0x80 <= b <= 0x8f:
            n = b & 0x0f
            out = {}
            for _ in range(n):
                k, pos = decode(pos)
                v, pos = decode(pos)
                out[k] = v
            return out, pos
        if 0x90 <= b <= 0x9f:
            n = b & 0x0f
            out = []
            for _ in range(n):
                v, pos = decode(pos)
                out.append(v)
            return out, pos
        if 0xa0 <= b <= 0xbf:
            raw, pos = read(pos, b & 0x1f)
            return raw.decode('utf-8'), pos
        if b == 0xc0:
            return None, pos
        if b == 0xc2:
            return False, pos
        if b == 0xc3:
            return True, pos
        if b in (0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3):
            fmts = {0xcc: '>B', 0xcd: '>H', 0xce: '>I', 0xcf: '>Q',
                    0xd0: '>b', 0xd1: '>h', 0xd2: '>i', 0xd3: '>q'}
            fmt = fmts[b]
            raw, pos = read(pos, struct.calcsize(fmt))
            return struct.unpack(fmt, raw)[0], pos
        if b in (0xd9, 0xda, 0xdb):
            fmts = {0xd9: '>B', 0xda: '>H', 0xdb: '>I'}
            fmt = fmts[b]
            raw_len, pos = read(pos, struct.calcsize(fmt))
            n = struct.unpack(fmt, raw_len)[0]
            raw, pos = read(pos, n)
            return raw.decode('utf-8'), pos
        if b in (0xdc, 0xdd):
            fmts = {0xdc: '>H', 0xdd: '>I'}
            fmt = fmts[b]
            raw_len, pos = read(pos, struct.calcsize(fmt))
            n = struct.unpack(fmt, raw_len)[0]
            out = []
            for _ in range(n):
                v, pos = decode(pos)
                out.append(v)
            return out, pos
        if b in (0xde, 0xdf):
            fmts = {0xde: '>H', 0xdf: '>I'}
            fmt = fmts[b]
            raw_len, pos = read(pos, struct.calcsize(fmt))
            n = struct.unpack(fmt, raw_len)[0]
            out = {}
            for _ in range(n):
                k, pos = decode(pos)
                v, pos = decode(pos)
                out[k] = v
            return out, pos
        raise ValueError(f'unsupported msgpack tag: 0x{b:02x}')
    val, end = decode(0)
    if end != len(mv):
        raise ValueError('trailing bytes in msgpack')
    return val

def _expected_index(m: dict, manifest_bytes: bytes) -> dict:
    recs = m['records']
    index = {k: m[k] for k in ('schema', 'target', 'layout_version', 'wave_size', 'layout_push_bytes', 'mesa_commit', 'patch_version')}
    index['manifest_hash'] = fnv(manifest_bytes)
    index['records'] = {k: {f: r[f] for f in ('spv_hash', 'code_hash', 'workgroup', 'layout', 'record')}
                        for k, r in recs.items()}
    return index

def _is_consistent_package(pkg_dir: Path, expected_target=None) -> bool:
    manifest_path = pkg_dir / 'manifest.json'
    msgpack_path = pkg_dir / 'manifest.msgpack'
    if not manifest_path.is_file() or not msgpack_path.is_file():
        return False
    try:
        manifest_bytes = manifest_path.read_bytes()
        msgpack_bytes = msgpack_path.read_bytes()
        if not manifest_bytes or not msgpack_bytes:
            return False
        m = json.loads(manifest_bytes.decode('utf-8'))
        recs = m.get('records')
        if not isinstance(recs, dict) or not recs or m.get('failures') != []:
            return False
        target = m.get('target')
        if (target not in TARGETS
                or (expected_target is not None and target != expected_target)
                or (pkg_dir.parent.name in TARGETS and target != pkg_dir.parent.name)):
            return False
        if (m.get('schema') != 1 or m.get('layout_version') != LAYOUT
                or m.get('wave_size') != 32 or m.get('layout_push_bytes') != 256
                or m.get('patch_version') not in (3, 4, 5)
                or not isinstance(m.get('mesa_commit'), str) or not m['mesa_commit']):
            return False
        for k, r in recs.items():
            if not isinstance(r, dict):
                return False
            wg = r.get('workgroup')
            if (r.get('spv_hash') != k or r.get('record') != f'{k}.nrp'
                    or not isinstance(r.get('code_hash'), str) or not r['code_hash']
                    or r.get('layout') not in ('views', 'sampled')
                    or not isinstance(wg, list) or len(wg) != 3
                    or not all(isinstance(x, int) and not isinstance(x, bool) and 0 < x <= 1024 for x in wg)):
                return False
        expected_idx = _expected_index(m, manifest_bytes)
        if _unpack_msgpack(msgpack_bytes) != expected_idx or msgpack_bytes != msgpack(expected_idx):
            return False
        disk_nrps = {p.name: p for p in pkg_dir.glob('*.nrp')}
        expected_nrps = {r['record'] for r in recs.values()}
        if set(disk_nrps) != expected_nrps:
            return False
        expected_mach = TARGETS[target]
        for r in recs.values():
            raw = disk_nrps[r['record']].read_bytes()
            if hashlib.sha256(raw).hexdigest() != r['record_sha256']:
                return False
            if len(raw) < 96 or struct.unpack_from('<I', raw, 76)[0] != expected_mach:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError, UnicodeDecodeError):
        return False

def _resolve_jobs(cli_jobs: int | None) -> int:
    if cli_jobs is not None:
        return cli_jobs
    env_val = os.environ.get('OPENRR_BUILD_JOBS', '').strip()
    if env_val:
        try:
            n = int(env_val)
            if n >= 1:
                return n
        except ValueError:
            pass
    return os.cpu_count() or 1


def _run_capture_pair(capture_bin: str, spv_str: str, env_pair: dict, pair_prefix: Path):
    """Execute one sentinel capture subprocess and read its generated artifacts."""
    for ext in ('.code', '.json', '.layouts'):
        pair_prefix.with_suffix(ext).unlink(missing_ok=True)
    result = subprocess.run([capture_bin, spv_str], env=env_pair, text=True, capture_output=True)
    log = result.stdout + result.stderr
    pair_prefix.with_suffix('.log').write_text(log)
    result.check_returncode()
    return (
        json.loads(pair_prefix.with_suffix('.json').read_text()),
        pair_prefix.with_suffix('.code').read_bytes(),
        log,
        pair_prefix.with_suffix('.layouts').read_bytes(),
    )


def _run_serial_captures_for_key(capture_bin: str, jobs_for_key: list[tuple[int, str, dict, Path, Path]]):
    """Run captures sequentially for duplicate-key inputs so shared raw prefixes never race."""
    out = {}
    for idx, spv_str, env_base, prefix_a, prefix_b in jobs_for_key:
        try:
            env_a = dict(env_base, ONR_ACO_CAPTURE=str(prefix_a.resolve()), ONR_ACO_ADDR32_HI='0x13579bdf')
            cap_a = _run_capture_pair(capture_bin, spv_str, env_a, prefix_a)
            env_b = dict(env_base, ONR_ACO_CAPTURE=str(prefix_b.resolve()), ONR_ACO_ADDR32_HI='0x2468ace1')
            cap_b = _run_capture_pair(capture_bin, spv_str, env_b, prefix_b)
            out[idx] = (cap_a, cap_b, None)
        except (ValueError, KeyError, OSError, subprocess.SubprocessError) as exc:
            out[idx] = (None, None, exc)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('inputs', nargs='+', type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--toolchain', type=Path, default=Path('build/aco-tools/toolchain.json'))
    ap.add_argument('--capture', type=Path, default=Path('build/aco-tools/aco_capture'))
    ap.add_argument('--precision', choices=('native', 'local', 'global'), default='local')
    ap.add_argument('--precision-split', action='store_true',
                    help='Capture *_lt/_lx variants at native precision; all other shaders keep --precision')
    ap.add_argument('--precision-native-only', nargs='*', default=[],
                    help='With --precision-split, restrict native precision to these exact stems; all other shaders keep --precision')
    ap.add_argument('--matrix-loads', choices=('scalar', 'vector8'), default='scalar')
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--capture-dir', type=Path, help='Build-only capture diagnostics (outside the package stage)')
    ap.add_argument('--cmat-address-shaders', nargs='*', default=[],
                    help='Exact shader stems using bounded cmat lane offsets; all others stay off')
    ap.add_argument('--cu-mode-shaders', nargs='*', default=[],
                    help='Compute kernels forced onto CU mode at compile time (rsrc1 bit 29 = 0)')
    ap.add_argument('--wgp-mode-shaders', nargs='*', default=[],
                    help='Compute kernels forced onto WGP mode at compile time (rsrc1 bit 29 = 1)')
    ap.add_argument('--sample-wait-zero-shaders', nargs='*', default=[],
                    help='Compute kernels coalescing GFX12 sampler waits to s_wait_samplecnt 0x0')
    ap.add_argument('--shared-cmat', choices=('off', 'load', 'loadstore'), default='off',
                    help='Vectorize same-lane contiguous shared-memory coopmat runs: loads only (load) or loads+stores (loadstore)')
    ap.add_argument('--shared-cmat-shaders', nargs='*', default=[],
                    help='Restrict --shared-cmat to these exact stems (empty: every shader, like before)')
    ap.add_argument('--target', choices=tuple(TARGETS), default='gfx1201',
                    help='AMDGPU capture target family (gfx1201=0x4e, gfx1200=0x48)')
    ap.add_argument('--jobs', '-j', type=int, default=None,
                    help='Maximum parallel aco_capture worker count (default: OPENRR_BUILD_JOBS or cpu count)')
    ns = ap.parse_args()
    if ns.jobs is not None and ns.jobs < 1:
        ap.error('--jobs must be >= 1')
    jobs = _resolve_jobs(ns.jobs)
    cfg = json.loads(ns.toolchain.read_text())
    expected_mach = TARGETS[ns.target]
    root = ns.out / ns.target / LAYOUT
    backup_root = root.parent / f'.{LAYOUT}.backup'
    root.parent.mkdir(parents=True, exist_ok=True)
    if not _is_consistent_package(root, ns.target) and _is_consistent_package(backup_root, ns.target):
        if root.exists():
            shutil.rmtree(root)
        os.replace(backup_root, root)
    elif _is_consistent_package(root, ns.target) and backup_root.exists():
        shutil.rmtree(backup_root, ignore_errors=True)
    had_consistent_root = _is_consistent_package(root, ns.target)
    manifest_path = root / 'manifest.json'
    try:
        manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    except (OSError, ValueError):
        manifest = {}
    if not isinstance(manifest, dict) or manifest.get('failures') != []:
        manifest = {}
    files = sorted({p.resolve() for arg in ns.inputs for p in (arg.rglob('*.spv') if arg.is_dir() else [arg])})
    stems = {p.stem for p in files}
    for label, selected in (('shared-cmat', ns.shared_cmat_shaders), ('cmat address', ns.cmat_address_shaders), ('cu-mode', ns.cu_mode_shaders),
                            ('wgp-mode', ns.wgp_mode_shaders), ('sample-wait-zero', ns.sample_wait_zero_shaders)):
        missing = set(selected) - stems
        if missing:
            ap.error(f'{label} shader inputs missing: ' + ', '.join(sorted(missing)))
    overlap = set(ns.cu_mode_shaders) & set(ns.wgp_mode_shaders)
    if overlap:
        ap.error('shaders cannot request both cu-mode and wgp-mode: ' + ', '.join(sorted(overlap)))
    if ns.cmat_address_shaders and not cfg.get('cmat_address_patch_sha256'):
        ap.error('selected toolchain does not declare the bounded cmat address patch')
    if ns.shared_cmat_shaders and ns.shared_cmat == 'off':
        ap.error('--shared-cmat-shaders needs --shared-cmat load or loadstore')
    # Read each input SPIR-V once up front so policy, split-precision, cache, and dimension checks share bytes.
    spv_items = []
    for idx, spv in enumerate(files):
        data = spv.read_bytes()
        spv_items.append((idx, spv, data, f'{fnv(data):016x}'))
    # Records are addressed by SPIR-V hash, so identical input bytes cannot
    # safely request conflicting per-stem policies. Reject instead of choosing
    # whichever path happens to be visited last.
    input_policies = {}
    for _, spv, _, key in spv_items:
        stem_pol = (spv.stem in ns.cmat_address_shaders, spv.stem in ns.cu_mode_shaders,
                    spv.stem in ns.wgp_mode_shaders, spv.stem in ns.sample_wait_zero_shaders,
                    spv.stem in ns.shared_cmat_shaders)
        if key in input_policies and input_policies[key] != stem_pol:
            ap.error('identical SPIR-V requests conflicting per-stem policies: ' + spv.name)
        input_policies[key] = stem_pol
    # Variant-split precision: fast variants (*_lt / *_lx) may run at native
    # precision while every legacy shader keeps the requested default, so
    # legacy records stay bit-identical. A fast SPIR-V whose hash is shared
    # with any legacy input is forced back onto the legacy policy.
    precision_by_stem = {}
    if ns.precision_split:
        def _is_fast(stem):
            return stem.endswith('_lt') or stem.endswith('_lx')
        native_only = set(ns.precision_native_only)
        legacy_keys = {key for _, spv, _, key in spv_items if not _is_fast(spv.stem)}
        for _, spv, _, key in spv_items:
            is_native = (spv.stem in native_only) if native_only else _is_fast(spv.stem)
            if not is_native or key in legacy_keys:
                precision_by_stem[spv.stem] = ns.precision
                if is_native:
                    print(f'{spv.name}: hash shared with a legacy input; keeping precision={ns.precision}', flush=True)
            else:
                precision_by_stem[spv.stem] = 'native'
    records = {}
    failures = []
    policy = dict(precision=ns.precision, matrix_loads=ns.matrix_loads, address_adaptation='assembler-dual-sentinel-v1')
    if ns.precision_split:
        policy['precision_split'] = dict(sorted(precision_by_stem.items()))
    if ns.shared_cmat != 'off':
        policy['shared_cmat'] = ns.shared_cmat
        if ns.shared_cmat_shaders:
            policy['shared_cmat_shaders'] = sorted(set(ns.shared_cmat_shaders))
    if ns.cmat_address_shaders:
        policy['cmat_address_shaders'] = sorted(set(ns.cmat_address_shaders))
    if ns.cu_mode_shaders:
        policy['cu_mode_shaders'] = sorted(set(ns.cu_mode_shaders))
    if ns.wgp_mode_shaders:
        policy['wgp_mode_shaders'] = sorted(set(ns.wgp_mode_shaders))
    if ns.sample_wait_zero_shaders:
        policy['sample_wait_zero_shaders'] = sorted(set(ns.sample_wait_zero_shaders))
    target_salt = b'' if ns.target == 'gfx1201' else (b'\0' + ns.target.encode())
    fingerprint = hashlib.sha256(ns.capture.read_bytes() + ns.toolchain.read_bytes() + Path(__file__).read_bytes() + json.dumps(policy,sort_keys=True).encode() + target_salt).hexdigest()
    stage_root = root.parent / f'.{LAYOUT}.staging.{os.getpid()}'
    shutil.rmtree(stage_root, ignore_errors=True)
    stage_root.mkdir(parents=True, exist_ok=True)
    raw = ns.capture_dir if ns.capture_dir else (stage_root / 'capture')
    raw.mkdir(parents=True, exist_ok=True)
    try:
        hit_records = {}
        miss_items = []
        base_env = dict(os.environ, VK_ICD_FILENAMES=cfg['icd'], LD_PRELOAD=cfg['shim'],
                        AMDGPU_GPU_ID=ns.target, MESA_SHADER_CACHE_DISABLE='true',
                        LD_LIBRARY_PATH=cfg['library_path'])
        base_env.pop('ONR_ACO_VECTOR_LOADS', None)
        base_env.pop('ONR_ACO_SHARED_CMAT', None)
        if ns.matrix_loads == 'vector8':
            base_env['ONR_ACO_VECTOR_LOADS'] = '1'
        for env_k in ('ONR_ACO_CMAT_ADDRESS', 'ONR_ACO_CU_MODE', 'ONR_ACO_WGP_MODE', 'ONR_ACO_SAMPLE_WAIT_ZERO'):
            base_env.pop(env_k, None)

        for idx, spv, data, key in spv_items:
            previous = manifest.get('records', {}).get(key)
            record_path = root/(key+'.nrp')
            stage_record_path = stage_root/(key+'.nrp')
            if not ns.refresh and manifest.get('target') == ns.target and manifest.get('fingerprint') == fingerprint and previous and record_path.exists():
                cached_bytes = record_path.read_bytes()
                if (previous.get('record_sha256') == hashlib.sha256(cached_bytes).hexdigest()
                        and len(cached_bytes) >= 96
                        and struct.unpack_from('<I', cached_bytes, 76)[0] == expected_mach):
                    stage_record_path.write_bytes(cached_bytes)
                    hit_records[idx] = previous
                    continue
            env = dict(base_env, ONR_ACO_PRECISION=precision_by_stem.get(spv.stem, ns.precision))
            for env_k, flag_set in (('ONR_ACO_CMAT_ADDRESS', ns.cmat_address_shaders),
                                    ('ONR_ACO_CU_MODE', ns.cu_mode_shaders),
                                    ('ONR_ACO_WGP_MODE', ns.wgp_mode_shaders),
                                    ('ONR_ACO_SAMPLE_WAIT_ZERO', ns.sample_wait_zero_shaders)):
                if spv.stem in flag_set:
                    env[env_k] = '1'
            if ns.shared_cmat != 'off' and (not ns.shared_cmat_shaders or spv.stem in ns.shared_cmat_shaders):
                env['ONR_ACO_SHARED_CMAT'] = ns.shared_cmat
            miss_items.append((idx, spv, data, key, env))

        key_counts = {}
        for _, _, _, key, _ in miss_items:
            key_counts[key] = key_counts.get(key, 0) + 1

        capture_bin = str(ns.capture.resolve())
        captured_by_idx = {}
        capture_errors = {}

        if miss_items:
            unique_misses = [m for m in miss_items if key_counts[m[3]] == 1]
            # Longest-job-first: heavy shaders start at t=0 across both sentinels.
            unique_misses.sort(key=lambda m: (-len(m[2]), m[0]))

            dup_groups = {}
            for m in miss_items:
                idx, spv, data, key, env = m
                if key_counts[key] > 1:
                    dup_groups.setdefault(key, []).append((idx, str(spv), env, raw / (key + '-a'), raw / (key + '-b')))

            pair_results = {}
            with concurrent.futures.ThreadPoolExecutor(max_workers=min(jobs, max(1, len(unique_misses) * 2 + len(dup_groups)))) as ex:
                all_futures = []
                pair_fut_meta = {}
                siblings = {}
                dup_futs = {}
                try:
                    for idx, spv, data, key, env in unique_misses:
                        spv_str = str(spv)
                        prefix_a = raw / (key + '-a')
                        prefix_b = raw / (key + '-b')
                        env_a = dict(env, ONR_ACO_CAPTURE=str(prefix_a.resolve()), ONR_ACO_ADDR32_HI='0x13579bdf')
                        env_b = dict(env, ONR_ACO_CAPTURE=str(prefix_b.resolve()), ONR_ACO_ADDR32_HI='0x2468ace1')
                        fa = ex.submit(_run_capture_pair, capture_bin, spv_str, env_a, prefix_a)
                        fb = ex.submit(_run_capture_pair, capture_bin, spv_str, env_b, prefix_b)
                        pair_fut_meta[fa] = (idx, 0)
                        pair_fut_meta[fb] = (idx, 1)
                        siblings[fa] = fb
                        siblings[fb] = fa
                        all_futures.extend((fa, fb))

                    dup_items = sorted(dup_groups.values(), key=lambda g: -len(g[0]))
                    for grp in dup_items:
                        fg = ex.submit(_run_serial_captures_for_key, capture_bin, grp)
                        dup_futs[fg] = grp
                        all_futures.append(fg)

                    pending = set(all_futures)
                    while pending:
                        done, pending = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
                        for f in done:
                            if f in dup_futs:
                                grp_out = f.result()
                                for idx, (cap_a, cap_b, exc) in grp_out.items():
                                    if exc is not None:
                                        capture_errors[idx] = exc
                                    else:
                                        captured_by_idx[idx] = (cap_a, cap_b)
                            else:
                                idx, pair_idx = pair_fut_meta[f]
                                try:
                                    res = f.result()
                                except (ValueError, KeyError, OSError, subprocess.SubprocessError) as exc:
                                    if idx not in capture_errors:
                                        capture_errors[idx] = exc
                                    sib = siblings[f]
                                    if sib.cancel():
                                        pending.discard(sib)
                                else:
                                    pair_results.setdefault(idx, {})[pair_idx] = res
                                    if 0 in pair_results[idx] and 1 in pair_results[idx]:
                                        captured_by_idx[idx] = (pair_results[idx][0], pair_results[idx][1])
                except BaseException:
                    for f in all_futures:
                        f.cancel()
                    concurrent.futures.wait(all_futures)
                    raise

        for idx, spv, data, key in spv_items:
            if idx in hit_records:
                records[key] = hit_records[idx]
                continue
            if idx in capture_errors:
                err = capture_errors[idx]
                failures.append(dict(spv=str(spv), error=str(err)))
                print(f'{spv.name}: FAILED: {err}', flush=True)
                continue
            try:
                (info, code, log, layouts), (other_info, other, other_log, other_layouts) = captured_by_idx[idx]
                if layouts != other_layouts: raise ValueError('descriptor layouts differ between sentinel captures')
                if info['wave_size'] != 32 or info['push_bytes'] > 244:
                    raise ValueError('unsupported wave size or push range')
                dims = dimensions(data)
                if dims[0]*dims[1]*dims[2] != info['workgroup_size']:
                    raise ValueError('workgroup mismatch')
                code, address_report = adapt_code(code, other, info['exec_bytes'], info, other_info, (log, other_log))
                record_data = pack(info, code, fnv(data), expected_mach)
                (stage_root/(key+'.nrp')).write_bytes(record_data)
                records[key] = dict(info, spv=spv.name, spv_hash=key, code_hash=f'{fnv(code):016x}',
                                    workgroup=list(dims), layout='views' if spv.stem.startswith('d3d_') else 'sampled',
                                    cmat_address=spv.stem in ns.cmat_address_shaders,
                                    cu_mode=spv.stem in ns.cu_mode_shaders,
                                    wgp_mode=spv.stem in ns.wgp_mode_shaders,
                                    sample_wait_zero=spv.stem in ns.sample_wait_zero_shaders,
                                    shared_cmat=ns.shared_cmat if (ns.shared_cmat != 'off' and (not ns.shared_cmat_shaders or spv.stem in ns.shared_cmat_shaders)) else 'off',
                                    record_sha256=hashlib.sha256(record_data).hexdigest(),
                                    address_report=address_report, record=key+'.nrp',
                                    precision=precision_by_stem.get(spv.stem, ns.precision))
                print(f'{spv.name}: {key} {len(code)} bytes, {info["vgprs"]} VGPR, set0={info["set0"]}', flush=True)
            except (ValueError, KeyError, OSError, subprocess.SubprocessError) as e:
                failures.append(dict(spv=str(spv), error=str(e)))
                print(f'{spv.name}: FAILED: {e}', flush=True)
        output = dict(schema=1, target=ns.target, layout_version=LAYOUT, wave_size=32, layout_push_bytes=256,
                      mesa_commit=cfg['mesa_commit'], patch_version=cfg['patch_version'], fingerprint=fingerprint,
                      compile_options=dict(subgroup=32, require_full_subgroups=True, features='gpu/src/vk.cpp Device::init', **policy),
                      records=records, failures=failures)
        failure_report = (ns.capture_dir if ns.capture_dir else (root / 'capture')) / 'failures.json'
        msgpack_path = root / 'manifest.msgpack'
        if failures:
            failure_report.parent.mkdir(parents=True, exist_ok=True)
            failure_report.write_text(json.dumps(output, indent=2) + '\n')
            if not had_consistent_root:
                manifest_path.unlink(missing_ok=True)
                msgpack_path.unlink(missing_ok=True)
            raise SystemExit(f'{len(failures)} capture(s) failed; see {failure_report}')
        failure_report.unlink(missing_ok=True)
        stage_manifest = stage_root / 'manifest.json'
        stage_manifest.write_text(json.dumps(output, indent=2)+'\n')
        # Runtime index uses the existing PAL MessagePack codec; bind it to the reviewable JSON.
        (stage_root / 'manifest.msgpack').write_bytes(msgpack(_expected_index(output, stage_manifest.read_bytes())))
        if not _is_consistent_package(stage_root, ns.target):
            raise ValueError('staged package failed consistency check')
        had_root_before_swap = root.exists()
        if had_root_before_swap and backup_root.exists():
            if _is_consistent_package(root, ns.target):
                shutil.rmtree(backup_root)
            else:
                raise RuntimeError(f'refusing to overwrite backup {backup_root} while {root} is inconsistent')
        published = False
        try:
            if had_root_before_swap:
                os.replace(root, backup_root)
            os.replace(stage_root, root)
            published = True
        except BaseException:
            if had_root_before_swap and backup_root.exists() and not root.exists():
                os.replace(backup_root, root)
            raise
        if published and _is_consistent_package(root, ns.target) and backup_root.exists():
            shutil.rmtree(backup_root, ignore_errors=True)
    finally:
        shutil.rmtree(stage_root, ignore_errors=True)

if __name__ == '__main__':
    main()
