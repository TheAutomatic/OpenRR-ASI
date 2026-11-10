# SPDX-License-Identifier: MIT
"""Pair Windows LLPC ELF resources with ACO records by identical SPIR-V bytes."""
from pathlib import Path
import struct,json,hashlib,sys,zipfile
import msgpack
sys.path.insert(0,'asi/build')
from build_aco_records import fnv
root=Path('build/aco-runtime-trace/hotspot-resources')
out=Path('docs/test-evidence/mesa-aco-next-optimization-2026-11-10')
out.mkdir(exist_ok=True)
def elf_info(path):
 b=path.read_bytes();assert b[:6]==bytes([127,69,76,70,2,1])
 offset=struct.unpack_from('<Q',b,40)[0];count,strings=struct.unpack_from('<HH',b,60)
 sections=[struct.unpack_from('<IIQQQQIIQQ',b,offset+64*i) for i in range(count)]
 sh=sections[strings];names=b[sh[4]:sh[4]+sh[5]];metadata=None;text_size=0;symbol_size=None
 for sh in sections:
  name=names[sh[0]:].split(bytes([0]),1)[0].decode()
  if name=='.text':text_size=sh[5]
  if sh[1]==7:
   pos=sh[4];end=pos+sh[5]
   while pos<end:
    ns,ds,t=struct.unpack_from('<III',b,pos);pos+=12
    note_name=b[pos:pos+ns].rstrip(bytes([0]));pos+=(ns+3)&~3
    data=b[pos:pos+ds];pos+=(ds+3)&~3
    if note_name==b'AMDGPU' and t==32:metadata=msgpack.unpackb(data,raw=False,strict_map_key=False)
  if sh[1]==2:
   st=sections[sh[6]];strings_b=b[st[4]:st[4]+st[5]]
   for pos in range(sh[4],sh[4]+sh[5],sh[9]):
    sn,si,other,index,value,size=struct.unpack_from('<IBBHQQ',b,pos)
    sym=strings_b[sn:].split(bytes([0]),1)[0]
    if sym==b'_amdgpu_cs_main':symbol_size=size
 assert metadata is not None and symbol_size is not None,path
 cs=metadata['amdpal.pipelines'][0]['.hardware_stages']['.cs']
 return {'vgprs':cs['.vgpr_count'],'sgprs':cs['.sgpr_count'],
         'scratch_bytes':cs.get('.scratch_memory_size',0),'lds_bytes':cs.get('.lds_size',0),
         'wave_size':cs.get('.wavefront_size'), 'workgroup':cs.get('.threadgroup_dimensions'),
         'entry_symbol_bytes':symbol_size,'text_section_bytes':text_size,'elf_bytes':len(b),
         'elf_sha256':hashlib.sha256(b).hexdigest(),'stage_metadata':cs}
rows=[]
for variant,listname,shaderdir,manifest in [
 ('original','hotspots.txt',Path('build/asi/stage/openrr-amd/shaders'),Path('build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1/manifest.json')),
 ('retained','retained.txt',root/'current-shaders',Path('build/aco-runtime-trace/experiment-enc96-wave-rcp/records/gfx1201/openrr-d3d12-v1/manifest.json')),
 ('address-retained','address-retained.txt',Path('build/aco-runtime-trace/experiment-address-screen/dec_dfn_96_128/candidate/data/shaders'),Path('build/aco-runtime-trace/experiment-address-screen/dec_dfn_96_128/candidate/records/gfx1201/openrr-d3d12-v1/manifest.json'))]:
 records=json.loads(manifest.read_text())['records']
 for name in (root/listname).read_text().splitlines():
  spv=(shaderdir/name).read_bytes();key=f'{fnv(spv):016x}';record=records[key]
  assert record['spv_hash']==key and record['spv']==name
  llpc=elf_info(root/variant/(name+'.elf'))
  assert llpc['workgroup']==record['workgroup'] and llpc['wave_size']==record['wave_size']
  rows.append({'variant':variant,'shader':name,'spv_fnv':key,'spv_sha256':hashlib.sha256(spv).hexdigest(),
    'llpc':llpc,'aco':{k:record[k] for k in ('vgprs','sgprs','scratch_bytes','shared_bytes','exec_bytes','code_hash','workgroup','wave_size')},
    'llpc_source':str(root/variant/(name+'.elf')),'aco_manifest':str(manifest)})
(out/'hotspot-resources.json').write_text(json.dumps(rows,indent=2)+chr(10))
lines=['# Same-SPIR-V hotspot resources — 2026-11-10','','Compile-only Windows LLPC driver ELF versus pinned ACO global/vector8 records. Each row pairs the exact SPIR-V hash; retained enc96 is a separate row. No GPU dispatch or timing in this capture.','','| Shader / variant | SPIR-V FNV | VGPR LLPC/ACO | Scratch B/thread LLPC/ACO | LDS B/group LLPC/ACO | Entry bytes LLPC/ACO |','|---|---|---:|---:|---:|---:|']
for r in rows:
 l=r['llpc'];a=r['aco'];lines.append(f"| {r['shader']} / {r['variant']} | {r['spv_fnv']} | {l['vgprs']}/{a['vgprs']} | {l['scratch_bytes']}/{a['scratch_bytes']} | {l['lds_bytes']}/{a['shared_bytes']} | {l['entry_symbol_bytes']}/{a['exec_bytes']} |")
lines+=['','LLPC entry size is the _amdgpu_cs_main ELF symbol size; ACO entry size is capture exec_bytes before the PAL adapter prologue. ELF/.text sizes and SGPR metadata are in JSON; .text padding, non-code ELF sections and the adapter are not counted as shader body. These are compiler-export metadata, not a direct observation of final Windows execution registers.','','LLPC exported ELF and device provenance are saved in hotspot-llpc-elf.zip; per-row ELF SHA256 and exact SPIR-V SHA256 are in JSON. ACO code identity is bound to its manifest and record.']
(out/'hotspot-resources.md').write_text(chr(10).join(lines)+chr(10))
with zipfile.ZipFile(out/'hotspot-llpc-elf.zip','w',zipfile.ZIP_DEFLATED) as z:
 for p in root.rglob('*'):
  if p.is_file() and (p.suffix=='.elf' or p.name=='device.txt'):z.write(p,p.relative_to(root).as_posix())
print(chr(10).join(lines))
