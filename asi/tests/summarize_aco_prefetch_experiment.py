# SPDX-License-Identifier: MIT
"""Summarize the isolated projection-prefetch experiment and archive small evidence."""
from pathlib import Path
import json,hashlib,statistics,shutil
runs=Path('build/aco-runtime-trace/runs-approved-2026-11-10')
experiment=Path('build/aco-runtime-trace/experiment-projection-prefetch')
out=Path('docs/test-evidence/mesa-aco-prefetch-experiment-2026-11-10')
out.mkdir(parents=True,exist_ok=True)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
checks={}
for label in ('baseline','candidate'):
 outputs=[]
 for phase,count in [('steady',8),('drs',12)]:
  for i in range(count):
   f=f'{label}-{phase}-{i}.bin'
   hashes=[digest(runs/name/f) for name in ('fast-aligned','fast-prefetch-control','fast-prefetch-trace-6')]
   assert len(set(hashes))==1,f
   outputs.append({'file':f,'sha256':hashes[0]})
 a=runs/'fast-trace-6'/f'{label}-trace-6';b=runs/'fast-prefetch-trace-6'/f'{label}-trace-6'
 assert {p.name for p in a.glob('*.bin')}=={p.name for p in b.glob('*.bin')}
 readbacks=[]
 for p in sorted(a.glob('*.bin')):
  h=digest(p);assert h==digest(b/p.name),p.name
  readbacks.append({'file':p.name,'sha256':h})
 checks[label]={'original_control_trace_outputs_equal':outputs,'original_candidate_trace_readbacks_equal':readbacks}
source=Path('build/asi/stage/openrr-amd/shaders');target=experiment/'data/shaders'
assert {p.name for p in source.glob('*.spv')}=={p.name for p in target.glob('*.spv')}
changed=[p.name for p in source.glob('*.spv') if digest(p)!=digest(target/p.name)]
assert changed==['dec_dfn_96_128_fl_lt.spv'],changed
rounds=[]
for directory in sorted((runs/'prefetch-timing').glob('round-*')):
 s=json.loads((directory/'summary.json').read_text());o=json.loads((directory/'order.json').read_text(encoding='utf-8-sig'))
 assert s['parity_passed']
 row={'round':o['round'],'first':o['baseline_role']}
 for role in ('baseline','candidate'):
  for phase in ('steady','drs'):
   row[o[role+'_role']+'_'+phase+'_ms']=s[role][phase+'_median_ms']
 rounds.append(row)
 dest=out/directory.name;dest.mkdir(exist_ok=True)
 for f in ('summary.json','order.json'):shutil.copy2(directory/f,dest/f)
stats={}
for phase in ('steady','drs'):
 stats[phase]={}
 for role in ('original','variant'):
  values=[r[role+'_'+phase+'_ms'] for r in rounds]
  stats[phase][role]={'median_ms':statistics.median(values),'min_ms':min(values),'max_ms':max(values)}
 stats[phase]['median_change_percent']=100*(stats[phase]['variant']['median_ms']/stats[phase]['original']['median_ms']-1)
 stats[phase]['paired_change_percent']=[100*(r['variant_'+phase+'_ms']/r['original_'+phase+'_ms']-1) for r in rounds]
result={'changed_shaders':changed,'correctness':checks,'rounds':rounds,'statistics':stats}
(out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
for name in ('instruction-comparison.json','source-change.patch'):shutil.copy2(experiment/name,out/name)
for name in ('fast-prefetch-control','fast-prefetch-trace-6'):
 d=out/name;d.mkdir(exist_ok=True)
 for p in (runs/name).glob('*'):
  if p.suffix in ('.json','.csv'):shutil.copy2(p,d/p.name)
shutil.copy2(runs/'prefetch-trace6-diff.json',out/'trace6-diff.json')
print(json.dumps({'rounds':rounds,'statistics':stats},indent=2))
