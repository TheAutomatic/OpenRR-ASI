from pathlib import Path
import struct,json
r=Path('build/aco-runtime-trace/runs-approved-2026-11-10')
out={}
for case in ('fast-attr9-full-fixed-trace-6','fast-attr9-prepared-trace-6'):
 out[case]={}
 for label in ('baseline','candidate'):
  a=r/'fast-trace-6'/f'{label}-trace-6';b=r/case/f'{label}-trace-6'
  rows=[]
  for name in ('input-previous-motion.bin','3-d3d_enc0_attr_f_lt-motion.bin','3-d3d_enc0_attr_f_lt-attributes.bin'):
   x,y=[(p/name).read_bytes() for p in (a,b)]
   extent=1280*720*8 if 'motion' in name else 1280*736*48
   diff=[i for i,(u,v) in enumerate(zip(x[:extent],y[:extent])) if u!=v]
   row=dict(name=name,differing_bytes=len(diff),first=diff[:12])
   if diff and 'motion' in name:
    i=diff[0]//2*2;row.update(xy=[i//8%1280,i//8//1280],channel=i//2%4,baseline_bits=x[i:i+2].hex(),candidate_bits=y[i:i+2].hex(),baseline=struct.unpack_from('<e',x,i)[0],candidate=struct.unpack_from('<e',y,i)[0])
   rows.append(row)
  out[case][label]=rows
print(json.dumps(out,indent=2))
(r/'motion-probe-diff.json').write_text(json.dumps(out,indent=2)+chr(10))
