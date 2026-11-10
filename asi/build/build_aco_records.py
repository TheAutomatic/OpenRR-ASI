#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture OpenRR SPIR-V with the pinned RADV and pack NRPAL002 records."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

LAYOUT = 'openrr-d3d12-v1'

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

def pack(info, code, spv_hash):
    pairs = info['inline']
    fields = [info[k] for k in ('rsrc1', 'rsrc2', 'rsrc3', 'exec_bytes', 'workgroup_size',
                                'shared_bytes', 'push_bytes', 'user_sgprs')]
    header = struct.pack('<8sQQ8I3i4I', b'NRPAL002', spv_hash, fnv(code), *fields,
                         info['set0'], info['push'], info['grid'], len(pairs), len(code),
                         0x4e, info['scratch_bytes'])
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

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('inputs', nargs='+', type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--toolchain', type=Path, default=Path('build/aco-tools/toolchain.json'))
    ap.add_argument('--capture', type=Path, default=Path('build/aco-tools/aco_capture'))
    ap.add_argument('--precision', choices=('native', 'local', 'global'), default='local')
    ap.add_argument('--matrix-loads', choices=('scalar', 'vector8'), default='scalar')
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--capture-dir', type=Path, help='Build-only capture diagnostics (outside the package stage)')
    ap.add_argument('--cmat-address-shaders', nargs='*', default=[],
                    help='Exact shader stems using bounded cmat lane offsets; all others stay off')
    ns = ap.parse_args()
    cfg = json.loads(ns.toolchain.read_text())
    root = ns.out / 'gfx1201' / LAYOUT
    root.mkdir(parents=True, exist_ok=True)
    raw = ns.capture_dir if ns.capture_dir else root / 'capture'
    raw.mkdir(parents=True, exist_ok=True)
    manifest_path = root/'manifest.json'
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    files = sorted({p.resolve() for arg in ns.inputs for p in (arg.rglob('*.spv') if arg.is_dir() else [arg])})
    missing_address_shaders = set(ns.cmat_address_shaders) - {p.stem for p in files}
    if missing_address_shaders:
        ap.error('cmat address shader inputs missing: ' + ', '.join(sorted(missing_address_shaders)))
    if ns.cmat_address_shaders and not cfg.get('cmat_address_patch_sha256'):
        ap.error('selected toolchain does not declare the bounded cmat address patch')
    # Records are addressed by SPIR-V hash, so identical input bytes cannot
    # safely request conflicting per-stem policies. Reject instead of choosing
    # whichever path happens to be visited last.
    input_policies = {}
    for spv in files:
        key = f'{fnv(spv.read_bytes()):016x}'
        address = spv.stem in ns.cmat_address_shaders
        if key in input_policies and input_policies[key] != address:
            ap.error('identical SPIR-V requests conflicting cmat address policies: ' + spv.name)
        input_policies[key] = address
    records = {}
    failures = []
    policy = dict(precision=ns.precision, matrix_loads=ns.matrix_loads, address_adaptation='assembler-dual-sentinel-v1')
    if ns.cmat_address_shaders:
        policy['cmat_address_shaders'] = sorted(set(ns.cmat_address_shaders))
    fingerprint = hashlib.sha256(ns.capture.read_bytes() + ns.toolchain.read_bytes() + Path(__file__).read_bytes() + json.dumps(policy,sort_keys=True).encode()).hexdigest()
    for spv in files:
        data = spv.read_bytes()
        key = f'{fnv(data):016x}'
        previous = manifest.get('records', {}).get(key)
        record_path = root/(key+'.nrp')
        if not ns.refresh and manifest.get('fingerprint') == fingerprint and previous and record_path.exists():
            if previous.get('record_sha256') == hashlib.sha256(record_path.read_bytes()).hexdigest():
                records[key] = previous
                continue
        prefix = raw/key
        try:
            env = dict(os.environ, VK_ICD_FILENAMES=cfg['icd'], LD_PRELOAD=cfg['shim'],
                       AMDGPU_GPU_ID='gfx1201', MESA_SHADER_CACHE_DISABLE='true',
                       LD_LIBRARY_PATH=cfg['library_path'], ONR_ACO_PRECISION=ns.precision)
            env.pop('ONR_ACO_VECTOR_LOADS', None)
            # Never inherit a global experimental setting into unrelated shaders.
            env.pop('ONR_ACO_CMAT_ADDRESS', None)
            if spv.stem in ns.cmat_address_shaders:
                env['ONR_ACO_CMAT_ADDRESS'] = '1'
            if ns.matrix_loads == 'vector8': env['ONR_ACO_VECTOR_LOADS'] = '1'
            captures=[]
            for suffix,sentinel in (('a', '0x13579bdf'), ('b', '0x2468ace1')):
                pair_prefix=raw/(key+'-'+suffix)
                for ext in ('.code','.json','.layouts'):
                    pair_prefix.with_suffix(ext).unlink(missing_ok=True)
                env.update(ONR_ACO_CAPTURE=str(pair_prefix.resolve()), ONR_ACO_ADDR32_HI=sentinel)
                result=subprocess.run([str(ns.capture.resolve()),str(spv)],env=env,text=True,capture_output=True)
                log=result.stdout+result.stderr
                pair_prefix.with_suffix('.log').write_text(log)
                result.check_returncode()
                captures.append((json.loads(pair_prefix.with_suffix('.json').read_text()),pair_prefix.with_suffix('.code').read_bytes(),log,pair_prefix.with_suffix('.layouts').read_bytes()))
            info,code,log,layouts=captures[0]
            other_info,other,other_log,other_layouts=captures[1]
            if layouts != other_layouts: raise ValueError('descriptor layouts differ between sentinel captures')
            if info['wave_size'] != 32 or info['push_bytes'] > 244:
                raise ValueError('unsupported wave size or push range')
            dims = dimensions(data)
            if dims[0]*dims[1]*dims[2] != info['workgroup_size']:
                raise ValueError('workgroup mismatch')
            code, address_report = adapt_code(code, other, info['exec_bytes'], info, other_info, (log,other_log))
            record_data = pack(info, code, fnv(data))
            record_path.write_bytes(record_data)
            records[key] = dict(info, spv=spv.name, spv_hash=key, code_hash=f'{fnv(code):016x}',
                                workgroup=list(dims), layout='views' if spv.stem.startswith('d3d_') else 'sampled',
                                cmat_address=spv.stem in ns.cmat_address_shaders,
                                record_sha256=hashlib.sha256(record_data).hexdigest(),
                                address_report=address_report, record=key+'.nrp')
            print(f'{spv.name}: {key} {len(code)} bytes, {info["vgprs"]} VGPR, set0={info["set0"]}', flush=True)
        except (ValueError, KeyError, OSError, subprocess.SubprocessError) as e:
            failures.append(dict(spv=str(spv), error=str(e)))
            print(f'{spv.name}: FAILED: {e}', flush=True)
    output = dict(schema=1, target='gfx1201', layout_version=LAYOUT, wave_size=32, layout_push_bytes=256,
                  mesa_commit=cfg['mesa_commit'], patch_version=cfg['patch_version'], fingerprint=fingerprint,
                  compile_options=dict(subgroup=32, require_full_subgroups=True, features='gpu/src/vk.cpp Device::init', **policy),
                  records=records, failures=failures)
    manifest_path.write_text(json.dumps(output, indent=2)+'\n')
    # Runtime index uses the existing PAL MessagePack codec; bind it to the reviewable JSON.
    index = {k: output[k] for k in ('schema', 'target', 'layout_version', 'wave_size', 'layout_push_bytes', 'mesa_commit', 'patch_version')}
    index['manifest_hash'] = fnv(manifest_path.read_bytes())
    index['records'] = {k: {f: r[f] for f in ('spv_hash', 'code_hash', 'workgroup', 'layout', 'record')}
                        for k, r in records.items()}
    (root/'manifest.msgpack').write_bytes(msgpack(index))
    if failures:
        raise SystemExit(f'{len(failures)} capture(s) failed; see {manifest_path}')

if __name__ == '__main__':
    main()
