#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
python3 asi/tests/prepare_attr9_trace.py
out=build/aco-runtime-trace/attr9-full
glslangValidator -V --target-env vulkan1.3 -DD3D_GUIDES=1 -DENC_F32=1 -DLT_ATTR_TAB=1 \
 -DLT_ATTR_RCP=1 -DLT_ATTR_DEPTH=1 -Igpu/shaders/include "$out/source/enc0_attr.comp" \
 -o "$out/data/shaders/d3d_enc0_attr_f_lt.spv"
python3 asi/build/build_aco_records.py "$out/data/shaders/d3d_enc0_attr_f_lt.spv" \
 --out "$out/records" --precision global --matrix-loads vector8
python3 - <<'PY'
import json,shutil,sys
from pathlib import Path
sys.path.insert(0,'asi/build')
from build_aco_records import msgpack,fnv
base=Path('build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1')
out=Path('build/aco-runtime-trace/attr9-full/records/gfx1201/openrr-d3d12-v1')
manifest=json.loads((base/'manifest.json').read_text())
probe=json.loads((out/'manifest.json').read_text())
for p in base.glob('*.nrp'):shutil.copy2(p,out/p.name)
manifest['records'].update(probe['records'])
manifest['diagnostic']='Full enc0_attr target pixel probe; not a performance/operator variant'
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+chr(10))
index={k:manifest[k] for k in ('schema','target','layout_version','wave_size','layout_push_bytes','mesa_commit','patch_version')}
index['manifest_hash']=fnv((out/'manifest.json').read_bytes())
index['records']={k:{f:r[f] for f in ('spv_hash','code_hash','workgroup','layout','record')} for k,r in manifest['records'].items()}
(out/'manifest.msgpack').write_bytes(msgpack(index))
PY
