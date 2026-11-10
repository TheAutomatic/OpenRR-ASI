from pathlib import Path
import struct,json,sys
r=Path('build/aco-runtime-trace/runs-approved-2026-11-10')
out={}
for label in ('baseline','candidate'):
 name=sys.argv[1] if len(sys.argv)>1 else 'fast-attr9-full-trace-6'
 a=r/'fast-trace-6'/f'{label}-trace-6';b=r/name/f'{label}-trace-6'
 active=1280*736*48
 attrs=[(p/'3-d3d_enc0_attr_f_lt-attributes.bin').read_bytes() for p in (a,b)]
 checks={}
 for p in a.glob('*.bin'):
  if p.name.endswith('attributes.bin'):continue
  aa,bb=p.read_bytes(),(b/p.name).read_bytes()
  if p.name.endswith('motion.bin'):aa,bb=aa[:1280*720*8],bb[:1280*720*8]
  checks[p.name]=aa==bb
 words=struct.unpack_from('<29I',attrs[1],active)
 out[label]={'original_active_attributes_equal':attrs[0][:active]==attrs[1][:active],
             'changed_original_readbacks':[n for n,equal in checks.items() if not equal],
             'probe_words':[hex(x) for x in words],
             'probe_float':[struct.unpack('<f',struct.pack('<I',x))[0] for x in words]}
 p=r/name
 out[label]['measured_hash_mismatches']=[n.name for n in (r/'fast-aligned').glob(f'{label}-*.bin') if n.read_bytes()!=(p/n.name).read_bytes()]
 print(label,json.dumps(out[label],indent=2))
(r/(name+'-inspection.json')).write_text(json.dumps(out,indent=2)+chr(10))
