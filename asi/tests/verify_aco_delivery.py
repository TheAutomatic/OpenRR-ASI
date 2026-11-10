# SPDX-License-Identifier: MIT
"""CPU package audit: complete inputs, tested runtime records and clean stage."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'build'))
from build_aco_records import fnv, msgpack

SELECTED = ['dec_dfn_96_128_fl_lt', 'enc_stage_128_128_160_fl_lt']
EXPECTED = {
    'enc_stage_96_128_128_fl_lt': ('f3817f3fb0a26545', 'c08996618a61cd08'),
    'dec_dfn_96_128_fl_lt': ('607b418f7ed5346e', '6a4ccda0cd2efb14'),
    'enc_stage_128_128_160_fl_lt': ('7637570aac01b415', '591c303d1916df59'),
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=Path('build/asi-delivery-candidate'))
    ap.add_argument('--tested', type=Path, default=Path('build/aco-runtime-trace/retained-combination/data'))
    ap.add_argument('--out', type=Path, required=True)
    ns = ap.parse_args()
    stage = ns.build/'stage'
    data = stage/'openrr-amd'
    rel = Path('aco/gfx1201/openrr-d3d12-v1')
    records = data/rel
    manifest = json.loads((records/'manifest.json').read_text())
    assert not manifest['failures']
    assert manifest['compile_options']['precision'] == 'global'
    assert manifest['compile_options']['matrix_loads'] == 'vector8'
    assert manifest['compile_options']['cmat_address_shaders'] == SELECTED
    assert manifest['mesa_commit'] == '96cb43121031992b85767f9c1be8f3f48e22b1d2'
    assert manifest['wave_size'] == 32 and manifest['patch_version'] == 4
    index = {k: manifest[k] for k in ('schema','target','layout_version','wave_size','layout_push_bytes','mesa_commit','patch_version')}
    index['manifest_hash'] = fnv((records/'manifest.json').read_bytes())
    index['records'] = {k: {f:r[f] for f in ('spv_hash','code_hash','workgroup','layout','record')}
                        for k,r in manifest['records'].items()}
    assert (records/'manifest.msgpack').read_bytes() == msgpack(index)
    files = sorted((data/'shaders').glob('*.spv'))
    assert {p.name for p in files} == {p.name for p in (ns.tested/'shaders').glob('*.spv')}
    runtime = []
    for path in files:
        spv = path.read_bytes()
        assert spv == (ns.tested/'shaders'/path.name).read_bytes(), path.name
        key = f'{fnv(spv):016x}'
        row = manifest['records'][key]
        assert row['cmat_address'] == (path.stem in SELECTED)
        body = (records/row['record']).read_bytes()
        assert body == (ns.tested/rel/row['record']).read_bytes(), path.name
        if path.stem in EXPECTED:
            assert (key, row['code_hash']) == EXPECTED[path.stem]
        runtime.append(dict(shader=path.name, spv=key, code=row['code_hash'], record_sha256=sha(body)))
    # Include non-runtime occupancy/B2 variants produced by the regular entry.
    all_inputs = list(files)
    for directory in ['occ_nopad','occ_pad16k','b2_base','b2_post_exp','b2_post_log','b2_attr_rcp','b2_attr_same','b2_c64_far3']:
        paths = list((ns.build/directory).glob('*.spv'))
        assert paths, directory
        all_inputs += paths
    keys = {f'{fnv(p.read_bytes()):016x}' for p in all_inputs}
    assert keys == set(manifest['records']), 'Missing or unexpected packaged records'
    for key, row in manifest['records'].items():
        record = (records/row['record']).read_bytes()
        assert sha(record) == row['record_sha256']
        magic, spv_hash, code_hash = struct.unpack_from('<8sQQ', record)
        assert magic == b'NRPAL002' and f'{spv_hash:016x}' == key
        pairs, code_size = struct.unpack_from('<II', record, 68)
        assert len(record) == 96 + 8*pairs + code_size
        assert fnv(record[96+8*pairs:]) == code_hash == int(row['code_hash'], 16)
    assert (stage/'openrr-amd.ini').read_bytes() == Path('asi/openrr-amd.ini').read_bytes()
    plugin = (stage/'plugins/openrr-amd.asi').read_bytes()
    assert plugin[:2] == b'MZ'
    pe = struct.unpack_from('<I', plugin, 60)[0]
    assert plugin[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', plugin, pe+4)[0] == 0x8664
    expected = {'plugins/openrr-amd.asi','openrr-amd.ini','LICENSE','THIRD_PARTY.md'}
    for name in ('LICENSE','THIRD_PARTY.md'):
        assert (stage/name).read_bytes() == Path(name).read_bytes()
    expected |= {p.relative_to(stage).as_posix() for p in files}
    expected |= {(records/name).relative_to(stage).as_posix() for name in ['manifest.json','manifest.msgpack']}
    expected |= {(records/r['record']).relative_to(stage).as_posix() for r in manifest['records'].values()}
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob('*') if p.is_file()}
    assert actual == expected, sorted(actual ^ expected)
    inventory = [dict(path=name, bytes=(stage/name).stat().st_size, sha256=sha((stage/name).read_bytes())) for name in sorted(actual)]
    result = dict(stage=str(stage), fingerprint=manifest['fingerprint'], runtime_count=len(runtime),
        record_count=len(keys), package_files=len(inventory), package_bytes=sum(r['bytes'] for r in inventory),
        compile_options=manifest['compile_options'], runtime=runtime, inventory=inventory,
        scope='CPU package audit; no GPU dispatch or performance measurement')
    ns.out.parent.mkdir(parents=True, exist_ok=True)
    ns.out.write_text(json.dumps(result, indent=2)+'\n')
    print(f'PASS: {len(runtime)} tested shaders/records, {len(keys)} total records, {len(inventory)} package files')

if __name__ == '__main__':
    main()
