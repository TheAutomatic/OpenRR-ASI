# SPDX-License-Identifier: MIT
from pathlib import Path
import json,statistics,hashlib,shutil,csv,re
root=Path('build/aco-runtime-trace/runs-approved-2026-11-10')
out=Path('docs/test-evidence/mesa-aco-current-comparison-2026-11-10');out.mkdir(exist_ok=True)
control=root/'fast-current-llpc-aco-control'
s=json.loads((control/'summary.json').read_text());assert s['parity_passed']
hashes={}
for p in sorted(control.glob('*.bin')):
 h=hashlib.sha256(p.read_bytes()).hexdigest()
 assert h==hashlib.sha256((root/'fast-aligned'/p.name).read_bytes()).hexdigest(),p.name
 hashes[p.name]=h
assert len(hashes)==40
rows=[]
for p in sorted((root/'llpc-aco-current-timing').glob('round-*')):
 s=json.loads((p/'summary.json').read_text());o=json.loads((p/'order.json').read_text(encoding='utf-8-sig'));assert s['parity_passed']
 states=[]
 for label in ('baseline','candidate'):
  log=(p/f'openrr_{label}.log').read_text()
  paths=re.findall(r'runtime paths: direct_guides=\d+ direct_output=\d+ sampler=\d+ flow=\d+',log)
  assert paths and all(v=='runtime paths: direct_guides=1 direct_output=1 sampler=1 flow=1' for v in paths)
  expected='ACO' if o[label+'_role']=='aco' else 'LLPC'
  entries=re.findall(r'compiler=(ACO|LLPC)\b',log)
  assert entries and set(entries)=={expected}
  with (p/f'{label}-frame-state.csv').open() as f:
   states.append([{k:v for k,v in row.items() if k!='call'} for row in csv.DictReader(f) if row['phase']!='building'])
 assert states[0]==states[1],p
 row={'round':o['round'],'first':o['baseline_role']}
 for label in ('baseline','candidate'):
  for phase in ('steady','drs'):
   row[o[label+'_role']+'_'+phase+'_ms']=s[label][phase+'_median_ms']
 rows.append(row)
 dest=out/p.name;dest.mkdir(exist_ok=True)
 for name in ('summary.json','order.json','baseline-frame-state.csv','candidate-frame-state.csv'):shutil.copy2(p/name,dest/name)
assert len(rows)==6
stats={}
for phase in ('steady','drs'):
 d={}
 for compiler in ('llpc','aco'):
  v=[r[compiler+'_'+phase+'_ms'] for r in rows]
  d[compiler]={'median_ms':statistics.median(v),'min_ms':min(v),'max_ms':max(v)}
 d['aco_time_increase_percent']=100*(d['aco']['median_ms']/d['llpc']['median_ms']-1)
 d['paired_time_increase_percent']=[100*(r['aco_'+phase+'_ms']/r['llpc_'+phase+'_ms']-1) for r in rows]
 stats[phase]=d
result={'rounds':rows,'statistics':stats,'control_output_sha256':hashes,'control_parity':json.loads((control/'summary.json').read_text())['parity']}
(out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
shutil.copy2(control/'summary.json',out/'control-summary.json')
for p in control.glob('*.csv'):shutil.copy2(p,out/p.name)
print(json.dumps({'rounds':rows,'statistics':stats},indent=2))
