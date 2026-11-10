#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
repo=$PWD
base=${OPENRR_MESA_SOURCE:-$HOME/src/mesa}
work=${OPENRR_MESA_WORK:-$HOME/work/openrr-mesa-aco}
commit=96cb43121031992b85767f9c1be8f3f48e22b1d2
[[ $(git -C "$base" rev-parse HEAD) == "$commit" ]] || { echo "Mesa source must be $commit" >&2; exit 1; }
mkdir -p "$work" build/aco-tools
if [[ ! -e $work/source ]]; then
    mkdir "$work/source"
    git -C "$base" archive "$commit" | tar -x -C "$work/source"
fi
for file in vulkan/radv_shader.c vulkan/radv_descriptor_set.c vulkan/radv_device.c vulkan/nir/radv_nir_lower_descriptors.c vulkan/nir/radv_nir_lower_cooperative_matrix.c compiler/aco_assembler.cpp; do
    git -C "$base" show "$commit:src/amd/$file" > "$work/source/src/amd/$file"
done
python3 asi/build/mesa/prepare_capture.py "$work/source"
python3 asi/build/mesa/prepare_global_address.py "$work/source"
export PKG_CONFIG_PATH="$HOME/src/mesa-deps/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ ! -e $work/build/build.ninja ]]; then
    meson setup "$work/build" "$work/source" --buildtype=release \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dtools=drm-shim \
        -Dplatforms= -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dvideo-codecs=
fi
ninja -C "$work/build" -j "${OPENRR_BUILD_JOBS:-8}"
g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include \
    asi/build/aco_capture.cpp gpu/src/vk.cpp -lvulkan -o build/aco-tools/aco_capture
python3 - "$work" "$repo" <<'PY'
import hashlib, json, pathlib, sys
w, r = map(pathlib.Path, sys.argv[1:])
icd = w/'build/src/amd/vulkan/radeon_devenv_icd.x86_64.json'
cfg = dict(mesa_commit='96cb43121031992b85767f9c1be8f3f48e22b1d2', patch_version=4,
           icd=str(icd), shim=str(w/'build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so'),
           library_path=str(pathlib.Path.home()/'src/mesa-deps/lib/x86_64-linux-gnu'))
cfg['radv_sha256'] = hashlib.sha256((w/'build/src/amd/vulkan/libvulkan_radeon.so').read_bytes()).hexdigest()
cfg['patch_sha256'] = hashlib.sha256((r/'asi/build/mesa/capture.patch').read_bytes()).hexdigest()
cfg['cmat_address_patch_sha256'] = hashlib.sha256((r/'asi/build/mesa/global-address.patch').read_bytes()).hexdigest()
cfg['cmat_address'] = 'bounded-lane-byte-offset-v1-opt-in'
(r/'build/aco-tools/toolchain.json').write_text(json.dumps(cfg, indent=2)+'\n')
PY
