# SPDX-License-Identifier: MIT
"""Offline compiler representations; never measures Linux GPU performance."""
from pathlib import Path
from collections import Counter
import os, json, subprocess, re
cfg=json.loads(Path('build/aco-tools/toolchain.json').read_text())
root=Path('build/aco-runtime-trace/experiment-projection-prefetch')
base=Path('build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1')
records=root/'records/gfx1201/openrr-d3d12-v1'
rows={}
for label,key,spv,record_root in [
 ('original','9b09c8e4e1c7b892',root/'reference/dec_dfn_96_128_fl_lt.spv',base),
 ('variant','19957c1b1dd28ad6',root/'data/shaders/dec_dfn_96_128_fl_lt.spv',records)]:
 out=(root/'ir'/label).resolve();out.mkdir(parents=True,exist_ok=True)
 env=dict(os.environ,VK_ICD_FILENAMES=cfg['icd'],LD_PRELOAD=cfg['shim'],
 AMDGPU_GPU_ID='gfx1201',MESA_SHADER_CACHE_DISABLE='true',LD_LIBRARY_PATH=cfg['library_path'],
 ONR_ACO_PRECISION='global',ONR_ACO_VECTOR_LOADS='1',ONR_ACO_CAPTURE=str(out/'capture'),
 ONR_ACO_ADDR32_HI='0x13579bdf',ONR_ISA=str(out))
 r=subprocess.run([str(Path('build/aco-tools/aco_capture').resolve()),str(spv.resolve())],env=env,text=True,capture_output=True)
 (out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
 assert (out/'capture.code').read_bytes()==(record_root/'capture'/(key+'-a.code')).read_bytes()
 text=next(out.glob('*.2.txt')).read_text().split('After lowering to hw instructions:',1)[1]
 counts=Counter()
 for line in text.splitlines():
  match=re.search(r'\b([sv]_[a-z0-9_]+|ds_[a-z0-9_]+|global_[a-z0-9_]+|scratch_[a-z0-9_]+|image_[a-z0-9_]+)\b',line)
  if match:counts[match[1]]+=1
 rows[label]={'spv_hash':key,'resources':json.loads((out/'capture.json').read_text()),'static_instruction_counts':dict(sorted(counts.items()))}
changes={op:[rows['original']['static_instruction_counts'].get(op,0),rows['variant']['static_instruction_counts'].get(op,0)] for op in sorted(set(rows['original']['static_instruction_counts'])|set(rows['variant']['static_instruction_counts'])) if rows['original']['static_instruction_counts'].get(op,0)!=rows['variant']['static_instruction_counts'].get(op,0)}
result={'captures':rows,'changed_static_counts':changes,'note':'Counts are static hw IR occurrences, not executed counts or timing.'}
(root/'instruction-comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(changes,indent=2))
