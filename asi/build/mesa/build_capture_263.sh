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

jobs=${OPENRR_BUILD_JOBS:-$(nproc 2>/dev/null || echo 8)}
if ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "invalid OPENRR_BUILD_JOBS: $jobs" >&2
    exit 1
fi

tmp_patch=""
tmp_obj=""
cleanup() {
    rm -rf -- "${tmp_patch:-}" "${tmp_obj:-}"
}
trap cleanup EXIT

tarball_sha256=$(sha256sum "$tarball" | awk '{print $1}')

patched=(src/amd/vulkan/radv_shader.c
         src/amd/vulkan/radv_descriptor_set.c
         src/amd/vulkan/nir/radv_nir_lower_cooperative_matrix.c
         src/amd/compiler/aco_assembler.cpp
         src/amd/compiler/aco_insert_waitcnt.cpp
         src/amd/compiler/aco_elf.cpp
         src/amd/common/amdgpu_devices.c
         src/amd/common/ac_gpu_info.h
         src/amd/common/ac_gpu_info.c
         src/amd/vulkan/radv_device.c)

patched_source_paths=()
tar_members=()
for f in "${patched[@]}"; do
    patched_source_paths+=("$source/$f")
    tar_members+=("mesa-26.3.0/$f")
done

radv_so=$build/src/amd/vulkan/libvulkan_radeon.so
drm_shim=$build/src/amd/drm-shim/libamdgpu_noop_drm_shim.so
icd_json=$build/src/amd/vulkan/radeon_devenv_icd.x86_64.json
stamp_file=$work/.openrr-patched.sha256

verify_patched_markers() {
    local cmat_src=$source/src/amd/vulkan/nir/radv_nir_lower_cooperative_matrix.c
    grep -q ONR_ACO_SHARED_CMAT "$cmat_src" || { echo "FATAL: ONR_ACO_SHARED_CMAT missing after patching" >&2; exit 1; }
    grep -q elem_bytes "$cmat_src" || { echo "FATAL: elem_bytes missing after patching" >&2; exit 1; }
    grep -q ONR_ACO_CU_MODE "$source/src/amd/vulkan/radv_shader.c" || { echo "FATAL: ONR_ACO_CU_MODE missing after patching" >&2; exit 1; }
    grep -q ONR_ACO_WGP_MODE "$source/src/amd/vulkan/radv_shader.c" || { echo "FATAL: ONR_ACO_WGP_MODE missing after patching" >&2; exit 1; }
    grep -q nir_float_muladd_support_fuse "$source/src/amd/vulkan/radv_shader.c" || { echo "FATAL: nir_float_muladd_support_fuse missing after patching" >&2; exit 1; }
    grep -q ONR_ACO_SAMPLE_WAIT_ZERO "$source/src/amd/compiler/aco_insert_waitcnt.cpp" || { echo "FATAL: ONR_ACO_SAMPLE_WAIT_ZERO missing after patching" >&2; exit 1; }
    grep -q '"gfx1200"' "$source/src/amd/common/amdgpu_devices.c" || { echo "FATAL: gfx1200 missing in amdgpu_devices.c" >&2; exit 1; }
    grep -q is_gfx1200 "$source/src/amd/compiler/aco_elf.cpp" || { echo "FATAL: is_gfx1200 missing in aco_elf.cpp" >&2; exit 1; }
}

all_mesa_inputs_exist() {
    [[ -e "$build/build.ninja" && -f "$radv_so" && -f "$drm_shim" && -f "$icd_json" ]] || return 1
    local p
    for p in "${patched_source_paths[@]}"; do
        [[ -f "$p" ]] || return 1
    done
    return 0
}

compute_mesa_stamp() {
    {
        printf '%s\n%s\n' "$pinned_commit" "$tarball_sha256"
        sha256sum \
            asi/build/mesa/prepare_capture.py \
            asi/build/mesa/prepare_global_address.py \
            asi/build/mesa/capture.patch \
            asi/build/mesa/global-address.patch \
            "${patched_source_paths[@]}" \
            "$radv_so" \
            "$drm_shim" \
            "$icd_json"
    } | sha256sum | awk '{print $1}'
}

if [[ -f "$stamp_file" ]] && all_mesa_inputs_exist && [[ "$(compute_mesa_stamp)" == "$(cat "$stamp_file")" ]]; then
    verify_patched_markers
    echo "[mesa-capture] Verified cached patched Mesa build ($stamp_file)"
else
    # Extract pristine copies into a scratch tree in one tarball pass, apply both
    # patchers there, and only overwrite $source/$f when bytes actually differ so
    # unchanged headers (e.g. ac_gpu_info.h) never trigger full Mesa rebuilds.
    tmp_patch=$(mktemp -d)
    tar -xJf "$tarball" -C "$tmp_patch" --strip-components=1 "${tar_members[@]}"
    python3 asi/build/mesa/prepare_capture.py "$tmp_patch"
    python3 asi/build/mesa/prepare_global_address.py "$tmp_patch"
    for f in "${patched[@]}"; do
        if [[ ! -f "$source/$f" ]] || ! cmp -s "$tmp_patch/$f" "$source/$f"; then
            mkdir -p "$(dirname -- "$source/$f")"
            cp -- "$tmp_patch/$f" "$source/$f"
        fi
    done
    rm -rf -- "$tmp_patch"
    tmp_patch=""
    verify_patched_markers

    export PKG_CONFIG_PATH="$HOME/src/mesa-deps/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    if [[ ! -e $build/build.ninja ]]; then
        meson setup "$build" "$source" --buildtype=release \
            -Dvulkan-drivers=amd -Dgallium-drivers= -Dtools=drm-shim \
            -Dplatforms= -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dvideo-codecs=
    fi
    ninja -C "$build" -j "$jobs" src/amd/vulkan/libvulkan_radeon.so src/amd/drm-shim/libamdgpu_noop_drm_shim.so
    if [[ ! -f "$icd_json" ]]; then
        ninja -C "$build" -j "$jobs"
    fi

    compute_mesa_stamp > "$stamp_file.tmp.$$"
    mv -f "$stamp_file.tmp.$$" "$stamp_file"
fi

# drirc: suppress the compute-queue custom-border-color state SGPR.
drirc_dir=$repo/build/mesa-comparison/mesa-263-drirc/drirc
mkdir -p "$drirc_dir" "$tools"
printf '%s\n' '<driconf>' '    <device driver="radv">' \
    '        <option name="radv_enable_custom_border_on_compute_queue" value="true"/>' \
    '    </device>' '</driconf>' > "$drirc_dir/radv-custom-border.conf"

aco_capture_bin=$tools/aco_capture
aco_stamp_file=$tools/.aco_capture.sha256
compute_aco_capture_stamp() {
    {
        g++ --version | head -n 1
        sha256sum \
            asi/build/aco_capture.cpp \
            gpu/src/vk.cpp \
            gpu/include/opennr_rr/*.hpp \
            gpu/src/*.hpp \
            "$aco_capture_bin"
    } | sha256sum | awk '{print $1}'
}

if [[ ! -x "$aco_capture_bin" || ! -f "$aco_stamp_file" ]] || [[ "$(compute_aco_capture_stamp)" != "$(cat "$aco_stamp_file")" ]]; then
    tmp_obj=$(mktemp -d)
    if (( jobs > 1 )); then
        g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include -c asi/build/aco_capture.cpp -o "$tmp_obj/aco_capture.o" &
        pid_cap=$!
        g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include -c gpu/src/vk.cpp -o "$tmp_obj/vk.o" &
        pid_vk=$!
        rc1=0
        rc2=0
        wait "$pid_cap" || rc1=$?
        wait "$pid_vk" || rc2=$?
        (( rc1 == 0 && rc2 == 0 )) || exit 1
    else
        g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include -c asi/build/aco_capture.cpp -o "$tmp_obj/aco_capture.o"
        g++ -std=c++20 -O2 -Iasi/third_party/vulkan/include -Igpu/include -c gpu/src/vk.cpp -o "$tmp_obj/vk.o"
    fi
    g++ -std=c++20 -O2 "$tmp_obj/aco_capture.o" "$tmp_obj/vk.o" -lvulkan -o "$aco_capture_bin.tmp.$$"
    mv -f "$aco_capture_bin.tmp.$$" "$aco_capture_bin"
    rm -rf -- "$tmp_obj"
    tmp_obj=""
    compute_aco_capture_stamp > "$aco_stamp_file.tmp.$$"
    mv -f "$aco_stamp_file.tmp.$$" "$aco_stamp_file"
fi

python3 - "$work" "$repo" "$tools" "$tarball_sha256" "$pinned_commit" "$drirc_dir" <<'PY'
import hashlib, json, pathlib, sys
w, r, tools, tarball_sha256, pinned, drirc_dir = sys.argv[1:7]
w, r, tools = map(pathlib.Path, (w, r, tools))
drirc_dir = pathlib.Path(drirc_dir)
version = (w/'mesa-26.3.0/VERSION').read_text().strip()
icd = w/'build/src/amd/vulkan/radeon_devenv_icd.x86_64.json'
cfg = dict(mesa_commit=pinned, mesa_version=version, mesa_release='release-tag-commit',
           patch_version=5,
           pinned_commit=pinned, release_tag='mesa-26.3.0',
           tarball_sha256=tarball_sha256,
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
