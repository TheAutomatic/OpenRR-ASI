#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Merge a diagnostic/experiment record set with immutable baseline records."""
from pathlib import Path
import sys,json,shutil
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'build'))
from build_aco_records import msgpack,fnv
base=Path('build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1')
root=Path(sys.argv[1])/'gfx1201/openrr-d3d12-v1'
manifest=json.loads((base/'manifest.json').read_text())
candidate=json.loads((root/'manifest.json').read_text())
assert candidate['compile_options']==manifest['compile_options']
for p in base.glob('*.nrp'):shutil.copy2(p,root/p.name)
manifest['records'].update(candidate['records'])
manifest['experiment']=sys.argv[1]
(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+chr(10))
index={k:manifest[k] for k in ('schema','target','layout_version','wave_size','layout_push_bytes','mesa_commit','patch_version')}
index['manifest_hash']=fnv((root/'manifest.json').read_bytes())
index['records']={k:{f:r[f] for f in ('spv_hash','code_hash','workgroup','layout','record')} for k,r in manifest['records'].items()}
(root/'manifest.msgpack').write_bytes(msgpack(index))
print('merged',len(manifest['records']),'records')
