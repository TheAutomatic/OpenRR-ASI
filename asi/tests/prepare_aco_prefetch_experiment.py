#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Isolated ACO experiment: shorten NT6 projection weight live ranges only."""
from pathlib import Path
import shutil,subprocess,sys,json,difflib
sys.path.insert(0,'asi/build')
from build_shaders import parse_shader_lists,parse_fastmode
from build_aco_records import fnv
root=Path('build/aco-runtime-trace/experiment-projection-prefetch')
source=root/'source';shutil.copytree('gpu/shaders',source,dirs_exist_ok=True)
p=source/'include/onr_block_w2.glsl'
s=p.read_text()
old='''            { MA p0, p1, q0, q1;
              OLD(p0, 0, 0) OLD(p1, 0, 1) OLD(q0, 1, 0) OLD(q1, 1, 1)
              OMM(0, p0, p1) OLD(p0, 2, 0) OLD(p1, 2, 1)
              OMM(1, q0, q1) OLD(q0, 3, 0) OLD(q1, 3, 1)
              OMM(2, p0, p1) OLD(p0, 4, 0) OLD(p1, 4, 1)
              OMM(3, q0, q1) OLD(q0, 5, 0) OLD(q1, 5, 1)
              OMM(4, p0, p1) OMM(5, q0, q1) }'''
new='''            // ACO experiment: same OLD addresses/MMA order; one pair live.
            { MA p0, p1;
              OLD(p0, 0, 0) OLD(p1, 0, 1) OMM(0, p0, p1)
              OLD(p0, 1, 0) OLD(p1, 1, 1) OMM(1, p0, p1)
              OLD(p0, 2, 0) OLD(p1, 2, 1) OMM(2, p0, p1)
              OLD(p0, 3, 0) OLD(p1, 3, 1) OMM(3, p0, p1)
              OLD(p0, 4, 0) OLD(p1, 4, 1) OMM(4, p0, p1)
              OLD(p0, 5, 0) OLD(p1, 5, 1) OMM(5, p0, p1) }'''
assert s.count(old)==1
p.write_text(s.replace(old,new))
name='dec_dfn_96_128'
flow=parse_shader_lists('gpu')[1];slot=flow.index(name)
defs=['ENC_F32=1','FLOW=1',f'FLOW_SLOT={slot}',f'FLOW_WAIT={int(slot>0)}']+parse_fastmode('gpu/fastmode.cmake')[name]
spvname=name+'_fl_lt.spv'
base=root/'reference';base.mkdir(exist_ok=True)
data=root/'data/shaders';data.mkdir(parents=True,exist_ok=True)
for f in Path('build/asi/stage/openrr-amd/shaders').glob('*.spv'):shutil.copy2(f,data/f.name)
for inc,src,dst in [('gpu/shaders/include',Path('gpu/shaders'),base/spvname),(str(source/'include'),source,data/spvname)]:
 subprocess.run(['glslangValidator','-V','--target-env','vulkan1.3',*[f'-D{d}' for d in defs],f'-I{inc}',str(src/(name+'.comp')),'-o',str(dst)],check=True)
baseline=Path('build/asi/stage/openrr-amd/shaders')/spvname
assert (base/spvname).read_bytes()==baseline.read_bytes(),'reference rebuild does not reproduce baseline SPIR-V'
(root/'source-change.patch').write_text(''.join(difflib.unified_diff(s.splitlines(True),s.replace(old,new).splitlines(True),fromfile='original/onr_block_w2.glsl',tofile='candidate/onr_block_w2.glsl')))
print(json.dumps({'baseline_spv':f'{fnv(baseline.read_bytes()):016x}','candidate_spv':f'{fnv((data/spvname).read_bytes()):016x}','defines':defs},indent=2))
