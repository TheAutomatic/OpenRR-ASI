# SPDX-License-Identifier: MIT
"""Freeze the isolated candidate, exact source/tool inventory and smoke evidence."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'docs/test-evidence/mesa-aco-delivery-2026-11-10'
BUILD = ROOT/'build/asi-delivery-candidate'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    inventory = {
        'production_source': [
            'gpu/shaders/enc_stage_96_128_128.comp', 'gpu/shaders/dec_dfn_96_128.comp',
            'gpu/shaders/enc_stage_128_128_160.comp', 'gpu/shaders/include/onr_block.glsl',
            'gpu/shaders/include/onr_block_fast.glsl', 'gpu/shaders/include/onr_block_w2.glsl'],
        'mesa_patch': ['asi/build/mesa/prepare_global_address.py','asi/build/mesa/global-address.patch'],
        'build_configuration': ['asi/build/build_asi.sh','asi/build/build_aco_records.py',
            'asi/build/mesa/build_capture.sh'],
        'retained_validation_tools': ['asi/build/build_runtime_trace.sh','asi/build/build_delivery_probe.sh',
            'asi/tests/run_runtime_trace_batch.ps1','asi/tests/run_aco_screen.ps1',
            'asi/tests/summarize_aco_screen.py','asi/tests/cleanup_aco_screen.ps1',
            'asi/tests/cmat_address_boundaries.py','asi/tests/capture_llpc_hotspots.cpp',
            'asi/tests/hotspot_resources.py','asi/tests/verify_retained_combination.py',
            'asi/tests/test_aco_build_policy.py','asi/tests/verify_aco_delivery.py',
            'asi/tests/run_aco_delivery_smoke.ps1','asi/tests/manage_aco_experiment_archive.py',
            'asi/tests/finalize_aco_delivery_evidence.py'],
        'documentation': ['README.md','THIRD_PARTY.md','asi/build/mesa/README.md',
            'docs/test-evidence/mesa-aco-delivery-2026-11-10.md'],
        # Existing committed foundation needed to reproduce the final combination.
        'unchanged_build_foundation': ['asi/build/mesa/prepare_capture.py','asi/build/mesa/capture.patch',
            'asi/build/aco_capture.cpp','asi/build/build_shaders.py','asi/build/BUILD_LOCAL_ASI.cmd',
            'asi/tests/compare_runtime_trace.py','asi/tests/test_runtime_trace_compare.py',
            'asi/tests/test_aco_records.py','asi/tests/aco_abi.comp','asi/tests/test_asi_units.cpp',
            'runtime/bench/openrr_bench.cpp','gpu/fastmode.cmake','gpu/shaders.list','gpu/CMakeLists.txt'],
    }
    archived = json.loads((OUT/'experiment-tool-list.json').read_text())['files']
    rows = {}
    for category, names in inventory.items():
        rows[category] = [dict(path=name, bytes=(ROOT/name).stat().st_size, sha256=sha((ROOT/name).read_bytes())) for name in names]
    rows['one_time_tools_archive'] = dict(manifest='experiment-tools.json', inventory='experiment-tool-list.json', count=len(archived))
    (OUT/'change-inventory.json').write_text(json.dumps(rows, indent=2)+'\n')
    for name in ['build.log','build-first-attempt.log','build-repeat.log','build-final.log','cpu-tests.log','cache-check.json','binary-exports-imports.txt']:
        src = ROOT/'build/aco-delivery-review'/name
        if src.exists():
            shutil.copy2(src, OUT/name)
    for name in ['package-smoke-final','package-smoke-delivery']:
        src = ROOT/'build/aco-delivery-review'/name
        if src.exists():
            shutil.copytree(src, OUT/name, dirs_exist_ok=True)
    shutil.copy2(ROOT/'build/aco-tools/toolchain.json', OUT/'toolchain.json')
    # A Windows-managed worktree has a Windows .git path; WSL Git cannot
    # resolve it. Generate this snapshot with host Git before freezing evidence.
    if not (OUT/'tracked-source.patch').is_file():
        raise FileNotFoundError('Run host git diff --binary HEAD --output=docs/test-evidence/mesa-aco-delivery-2026-11-10/tracked-source.patch')
    members = []
    archive = OUT/'candidate-stage.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted((BUILD/'stage').rglob('*')):
            if path.is_file():
                data = path.read_bytes()
                name = path.relative_to(BUILD/'stage').as_posix()
                z.writestr(name, data)
                members.append(dict(path=name, bytes=len(data), sha256=sha(data)))
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for row in members:
            assert sha(z.read(row['path'])) == row['sha256']
    (OUT/'candidate-stage.json').write_text(json.dumps(dict(sha256=sha(archive.read_bytes()), bytes=archive.stat().st_size, members=members), indent=2)+'\n')
    sources = OUT/'delivery-source-tools.zip'
    with zipfile.ZipFile(sources, 'w', zipfile.ZIP_DEFLATED) as z:
        for names in inventory.values():
            for name in names:
                z.write(ROOT/name, name)
        z.write(OUT/'change-inventory.json', 'change-inventory.json')
    (OUT/'source-archive.json').write_text(json.dumps(dict(path=sources.name, sha256=sha(sources.read_bytes()), bytes=sources.stat().st_size), indent=2)+'\n')
    print(f'Frozen {len(members)} stage files, {sum(map(len, inventory.values()))} source/tool inputs; no commit or stage replacement')

if __name__ == '__main__':
    main()
