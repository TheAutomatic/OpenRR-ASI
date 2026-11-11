#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Mesa 26.3.0-drirc capture toolchain (production).
# Companion to build_capture.sh, which pins the 26.2.4 commit 96cb43121031…
# from a git checkout. 26.3.0 arrives as a release tarball: provenance is the
# mesa-26.3.0 release tag commit (the tarball is generated from that tag) plus
# the tarball SHA256; both are folded into every manifest fingerprint. The
# capture runs with DRIRC_CONFIGDIR pointing at a drirc file that sets
# radv_enable_custom_border_on_compute_queue=true, which keeps AC_UD_CS_STATE
# out of the records (radv_shader_args.c radv_cs_needs_state_sgpr adds that
# user SGPR only when the option is false and the shader uses a sampler;
# OpenRR never uses custom border colors). Meson/Ninja options and
# dependencies are identical to the 26.2.4 build.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
repo=$PWD
work=${OPENRR_MESA_263_WORK:-$HOME/work/openrr-mesa-263-comparison}
source=${OPENRR_MESA_263_SOURCE:-$work/mesa-26.3.0}
build=$work/build
tools=$repo/build/mesa-comparison/mesa-263-drirc-toolchain
tarball=${OPENRR_MESA_263_TARBALL:-/mnt/c/Users/Kenny/Downloads/OpenRR/build/mesa-version-review/mesa-26.3.0.tar.xz}
# mesa-26.3.0 release tag commit (gitlab.freedesktop.org/mesa/mesa, tag mesa-26.3.0)
pinned_commit=0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4
[[ -d $source ]] || { echo "missing Mesa 26.3.0 source at $source" >&2; exit 1; }
[[ -f $tarball ]] || { echo "missing Mesa 26.3.0 tarball at $tarball" >&2; exit 1; }

# Capture patch: apply once; refuse silently inconsistent state.
if ! grep -q onr_capture "$source/src/amd/vulkan/radv_shader.c"; then
    python3 asi/build/mesa/prepare_capture.py "$source"
fi
# Bounded cmat address experiment: apply once, regenerate global-address.patch.
if ! grep -q ONR_ACO_CMAT_ADDRESS "$source/src/amd/vulkan/nir/radv_nir_lower_cooperative_matrix.c"; then
    python3 asi/build/mesa/prepare_global_address.py "$source"
fi

# drirc: suppress the compute-queue custom-border-color state SGPR.
drirc_dir=$repo/build/mesa-comparison/mesa-263-drirc/drirc
mkdir -p "$drirc_dir" "$tools"
printf '%s\n' '<driconf>' '    <device driver="radv">' \
    '        <option name="radv_enable_custom_border_on_compute_queue" value="true"/>' \
    '    </device>' '</driconf>' > "$drirc_dir/radv-custom-border.conf"

export PKG_CONFIG_PATH="$HOME/src/mesa-deps/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [[ ! -e $build/build.ninja ]]; then
    meson setup "$build" "$source" --buildtype=release \
        -Dvulkan-drivers=amd -Dgallium-drivers= -Dtools=drm-shim \
        -Dplatforms= -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dvideo-codecs=
fi
ninja -C "$build" -j "${OPENRR_BUILD_JOBS:-8}"
g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include \
    asi/build/aco_capture.cpp gpu/src/vk.cpp -lvulkan -o "$tools/aco_capture"

python3 - "$work" "$repo" "$tools" "$tarball" "$pinned_commit" "$drirc_dir" <<'PY'
import hashlib, json, pathlib, sys
w, r, tools, tarball, pinned, drirc_dir = sys.argv[1:7]
w, r, tools, tarball = map(pathlib.Path, (w, r, tools, tarball))
drirc_dir = pathlib.Path(drirc_dir)
version = (w/'mesa-26.3.0/VERSION').read_text().strip()
icd = w/'build/src/amd/vulkan/radeon_devenv_icd.x86_64.json'
cfg = dict(mesa_commit=pinned, mesa_version=version, mesa_release='release-tag-commit',
           patch_version=4,
           pinned_commit=pinned, release_tag='mesa-26.3.0',
           tarball_sha256=hashlib.sha256(tarball.read_bytes()).hexdigest(),
           icd=str(icd), shim=str(w/'build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so'),
           library_path=str(pathlib.Path.home()/'src/mesa-deps/lib/x86_64-linux-gnu'))
cfg['radv_sha256'] = hashlib.sha256((w/'build/src/amd/vulkan/libvulkan_radeon.so').read_bytes()).hexdigest()
cfg['patch_sha256'] = hashlib.sha256((r/'asi/build/mesa/capture.patch').read_bytes()).hexdigest()
cfg['cmat_address_patch_sha256'] = hashlib.sha256((r/'asi/build/mesa/global-address.patch').read_bytes()).hexdigest()
cfg['cmat_address'] = 'bounded-lane-byte-offset-v1-opt-in'
conf = drirc_dir/'radv-custom-border.conf'
cfg['drirc'] = {'radv_enable_custom_border_on_compute_queue': True,
  'purpose': 'suppress AC_UD_CS_STATE user SGPR on sampler compute kernels (radv_shader_args.c radv_cs_needs_state_sgpr)',
  'conf_dir': str(conf.parent.relative_to(r)), 'conf_sha256': hashlib.sha256(conf.read_bytes()).hexdigest(),
  'injection': 'DRIRC_CONFIGDIR environment for aco_capture'}
(tools/'toolchain.json').write_text(json.dumps(cfg, indent=2)+'\n')
PY
echo "toolchain: $tools/toolchain.json"
