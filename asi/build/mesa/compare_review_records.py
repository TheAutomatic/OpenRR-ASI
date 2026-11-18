#!/usr/bin/env python3
"""Summarize compiler-only differences without running a GPU workload."""
import json
from pathlib import Path

root=Path('build/aco-review-v4')
policies=('global-vector8','global-scalar','local-vector8','local-scalar','native-scalar')
manifests={p:json.loads((root/p/'gfx1201/openrr-d3d12-v1/manifest.json').read_text()) for p in policies}
keys=set(manifests[policies[0]]['records'])
assert all(set(m['records'])==keys and not m['failures'] for m in manifests.values())
report={'record_count':len(keys),'policies':{},'comparisons':{}}
for p,m in manifests.items():
    r=m['records']
    report['policies'][p]={'options':m['compile_options'],
        'scratch_records':sum(x['scratch_bytes']>0 for x in r.values()),
        'code_bytes':sum(x['exec_bytes'] for x in r.values())}
for a,b in (('global-vector8','local-vector8'),('global-scalar','local-scalar'),
            ('local-scalar','native-scalar'),('global-vector8','global-scalar')):
    differences=[]
    for k in sorted(keys):
        ra=manifests[a]['records'][k];rb=manifests[b]['records'][k]
        pa=root/a/'gfx1201/openrr-d3d12-v1'/ra['record']
        pb=root/b/'gfx1201/openrr-d3d12-v1'/rb['record']
        if pa.read_bytes()!=pb.read_bytes():
            differences.append({'key':k,'shader':ra['spv'],
                'a':{f:ra[f] for f in ('code_hash','exec_bytes','vgprs','scratch_bytes')},
                'b':{f:rb[f] for f in ('code_hash','exec_bytes','vgprs','scratch_bytes')}})
    report['comparisons'][a+'__'+b]={'identical_records':len(keys)-len(differences),'differences':differences}
path=root/'compiler-comparison.json'
path.write_text(json.dumps(report,indent=2)+'\n')
print('records:',len(keys))
for pair,result in report['comparisons'].items():
    print(pair,'identical:',result['identical_records'],'different:',len(result['differences']))
