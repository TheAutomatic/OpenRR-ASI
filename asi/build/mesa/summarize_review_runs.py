#!/usr/bin/env python3
"""Summarize isolated review benchmark runs without starting GPU work."""
import json
from pathlib import Path
import statistics

root=Path('build/aco-review-v4')
policies=('global-vector8','local-vector8','global-scalar','local-scalar','native-scalar')
result={}
for p in policies:
    runs=[]
    for n in range(1,4):
        path=root/f'idle-speed-r{n}-{p}'/'summary.json'
        if path.exists():
            s=json.loads(path.read_text())
            runs.append(dict(round=n,baseline=s['baseline'],candidate=s['candidate'],
                             parity_passed=s['parity_passed'],parity=s['parity']))
    if not runs: continue
    base=statistics.median(r['baseline']['steady_median_ms'] for r in runs)
    cand=statistics.median(r['candidate']['steady_median_ms'] for r in runs)
    drs_base=statistics.median(r['baseline']['drs_median_ms'] for r in runs)
    drs_cand=statistics.median(r['candidate']['drs_median_ms'] for r in runs)
    result[p]=dict(runs=runs,baseline_steady=base,candidate_steady=cand,
        increase_percent=100*(cand/base-1),baseline_drs=drs_base,candidate_drs=drs_cand,
        range_ms=[min(r['candidate']['steady_median_ms'] for r in runs),max(r['candidate']['steady_median_ms'] for r in runs)])
    print(f'{p}: n={len(runs)} LLPC={base:.4f} ACO={cand:.4f} delta={100*(cand/base-1):.1f}% DRS={drs_base:.4f}/{drs_cand:.4f}')
(root/'idle-speed-summary.json').write_text(json.dumps(result,indent=2)+'\n')
