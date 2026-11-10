# SPDX-License-Identifier: MIT
"""Compare rebuilt production inputs/records against individually tested combination."""
from pathlib import Path
import json,hashlib,sys
sys.path.insert(0,'asi/build')
from build_aco_records import fnv
root=Path('build/aco-runtime-trace/retained-combination/data')
tested=Path('build/aco-runtime-trace/experiment-enc128-unroll/candidate/data')
rows=[]
for p in sorted((root/'shaders').glob('*.spv')):
 q=tested/'shaders'/p.name
 assert p.read_bytes()==q.read_bytes(),p.name
 rows.append({'shader':p.name,'spv_fnv':f'{fnv(p.read_bytes()):016x}','sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
recorddir='aco/gfx1201/openrr-d3d12-v1'
m=json.loads((root/recorddir/'manifest.json').read_text())
tm=json.loads((tested/recorddir/'manifest.json').read_text())
records=[]
for key,r in m['records'].items():
 assert key in tm['records'] and r['code_hash']==tm['records'][key]['code_hash'],r['spv']
 p=root/recorddir/(key+'.nrp');q=tested/recorddir/(key+'.nrp')
 assert p.read_bytes()==q.read_bytes(),r['spv']
 records.append({'shader':r['spv'],'spv':key,'code':r['code_hash'],'record_sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
out=Path('docs/test-evidence/mesa-aco-enc128-unroll-2026-11-10')
(out/'combination-build.json').write_text(json.dumps({'shaders':rows,'records':records,'compile_options':m['compile_options']},indent=2)+chr(10))
print(f'Combination rebuild: {len(rows)} SPIR-V and {len(records)} records equal tested combination')
