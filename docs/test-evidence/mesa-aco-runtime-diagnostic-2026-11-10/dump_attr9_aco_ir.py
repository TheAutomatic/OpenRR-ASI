from pathlib import Path
import json,os,subprocess
cfg=json.loads(Path('build/aco-tools/toolchain.json').read_text())
out=Path('build/aco-runtime-trace/attr9-aco-ir').resolve();out.mkdir(exist_ok=True)
env=dict(os.environ,VK_ICD_FILENAMES=cfg['icd'],LD_PRELOAD=cfg['shim'],AMDGPU_GPU_ID='gfx1201',MESA_SHADER_CACHE_DISABLE='true',LD_LIBRARY_PATH=cfg['library_path'],ONR_ACO_PRECISION='global',ONR_ACO_VECTOR_LOADS='1',ONR_ACO_CAPTURE=str(out/'capture'),ONR_ACO_ADDR32_HI='0x13579bdf',ONR_ISA=str(out))
r=subprocess.run([str(Path('build/aco-tools/aco_capture').resolve()),str(Path('build/asi/stage/openrr-amd/shaders/d3d_enc0_attr_f_lt.spv').resolve())],env=env,text=True,capture_output=True)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
assert (out/'capture.code').read_bytes()==Path('build/aco-review-v4/global-vector8/gfx1201/openrr-d3d12-v1/capture/1d6fd9f4cc1c893c-a.code').read_bytes()
print('offline capture body matches baseline; compiler representations:',[p.name for p in out.glob('*.txt')])
