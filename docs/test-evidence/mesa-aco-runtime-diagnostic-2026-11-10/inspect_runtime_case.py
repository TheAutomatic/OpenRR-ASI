import json,re,struct
from pathlib import Path
root=Path('build/aco-runtime-trace/runs-approved-2026-11-10')
out={}
for frame in range(9):
 p=root/f'fast-trace-{frame}'
 if not p.exists():continue
 a,b=[(p/f'{label}-trace-{frame}'/'trace.txt').read_text() for label in ('baseline','candidate')]
 views=[re.findall(r'^view (.+)$',s,re.M) for s in (a,b)]
 descriptors=[re.findall(r'^sampled-descriptors=(.+)$',s,re.M) for s in (a,b)]
 j=json.loads((root/f'trace{frame}-diff.json').read_text()) if (root/f'trace{frame}-diff.json').exists() else {}
 out[str(frame)]={'view_metadata_equal':views[0]==views[1], 'sampled_descriptors_equal':descriptors[0]==descriptors[1],
 'pending_push_addresses':len(j.get('metadata_validation',{}).get('pending_address_pairs',[])),
 'differing_buffers':[r for r in j.get('buffers',[]) if r['differing_bytes']]}
p=root/'fast-trace-6'
a,b=[(p/f'{label}-trace-6'/'3-d3d_enc0_attr_f_lt-attributes.bin').read_bytes() for label in ('baseline','candidate')]
i=23234111; gid,byte=divmod(i,48)
bits=a[i]^b[i]
out['attribute_case']={'byte':i,'gid':gid,'xy':[gid%1280,gid//1280],'record_byte':byte,'baseline_byte':a[i],'candidate_byte':b[i],
 'xor':bits,'attributes':[8+16*(bit//4)+2*(bit%4)+1 for bit in range(8) if bits>>bit&1],
 'baseline_record':a[gid*48:(gid+1)*48].hex(),'candidate_record':b[gid*48:(gid+1)*48].hex()}
out['zero_cases']=[]
for frame in (0,1,3):
 p=root/f'fast-trace-{frame}'
 fs=[next((p/f'{label}-trace-{frame}').glob('*-tile.bin')).read_bytes() for label in ('baseline','candidate')]
 out['zero_cases'].append({'frame':frame,'record_half_offsets':sorted(set((i//2)%12 for i,(x,y) in enumerate(zip(*fs)) if x!=y))})
(root/'case-inspection.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out['attribute_case']))
print(out['zero_cases'])
for f,v in out.items():
 if f.isdigit():print(f,{k:v[k] for k in ('view_metadata_equal','sampled_descriptors_equal','pending_push_addresses')})
