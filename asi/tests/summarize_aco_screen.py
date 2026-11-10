# SPDX-License-Identifier: MIT
"""Archive screened candidate correctness and timings without large readbacks."""
from pathlib import Path
import json,hashlib,statistics,shutil,sys
name=sys.argv[1];root=Path('build/aco-runtime-trace/runs-next-optimization-2026-11-10')/name
out=Path('docs/test-evidence/mesa-aco-next-optimization-2026-11-10')/name;out.mkdir(exist_ok=True)
previous=json.loads((out/'results.json').read_text()) if (out/'results.json').exists() else {}
result={}
for stage in ('Correctness','Short','LongCorrectness','Long'):
 rows=[]
 for p in sorted((root/stage).glob('round-*')):
  if not (p/'summary.json').exists() or not (p/'order.json').exists():
   print(f'Pending run excluded: {p}', file=sys.stderr)
   continue
  s=json.loads((p/'summary.json').read_text());o=json.loads((p/'order.json').read_text(encoding='utf-8-sig'))
  assert s['parity_passed']
  row={'round':o['round'],'first':o['baseline_role'],'parity':s['parity']}
  for label in ('baseline','candidate'):
   for phase in ('steady','drs'):row[o[label+'_role']+'_'+phase+'_ms']=s[label][phase+'_median_ms']
  if stage in ('Correctness','LongCorrectness'):
   row['output_hashes']=[]
   for phase,count in [('steady',280 if stage=='LongCorrectness' else 8),('drs',12)]:
    for i in range(count):
     a=p/f'baseline-{phase}-{i}.bin';b=p/f'candidate-{phase}-{i}.bin'
     if a.exists() and b.exists():
      hashes=[hashlib.sha256(x.read_bytes()).hexdigest() for x in (a,b)]
     else:
      saved=next(r for r in previous[stage]['rounds'] if r['round']==o['round'])
      saved=next(r for r in saved['output_hashes'] if r['phase']==phase and r['index']==i)
      hashes=[saved['original'],saved['candidate']]
     row['output_hashes'].append({'phase':phase,'index':i,'original':hashes[0],'candidate':hashes[1],'equal':hashes[0]==hashes[1]})
  rows.append(row)
  dest=out/stage/p.name;dest.mkdir(parents=True,exist_ok=True)
  for f in p.iterdir():
   if f.suffix in ('.json','.csv','.log'):shutil.copy2(f,dest/f.name)
 if rows:
  stats={}
  if stage not in ('Correctness','LongCorrectness'):
   for phase in ('steady','drs'):
    stats[phase]={}
    for role in ('original','variant'):
     v=[r[role+'_'+phase+'_ms'] for r in rows];stats[phase][role]={'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v)}
    stats[phase]['change_percent']=100*(stats[phase]['variant']['median_ms']/stats[phase]['original']['median_ms']-1)
    stats[phase]['paired_change_percent']=[100*(r['variant_'+phase+'_ms']/r['original_'+phase+'_ms']-1) for r in rows]
  result[stage]={'rounds':rows,'statistics':stats}
(out/'results.json').write_text(json.dumps(result,indent=2)+chr(10))
print(json.dumps({k:v['statistics'] for k,v in result.items()},indent=2))
