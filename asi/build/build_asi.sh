#!/usr/bin/env bash
# Builds standalone openrr-amd.asi (OptiScaler plugin with embedded OpenRR runtime + GPU engine) via MinGW-w64.
# Usage: bash asi/build/build_asi.sh [build_dir]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=$(realpath -m -- "${1:-build/asi}")
stage="$out/stage"
if [[ "${OPENRR_CLEAN_STAGE:-0}" == "1" ]]; then
    rm -rf -- "$stage"
fi
rm -rf -- "$stage/plugins" "$stage/openrr-ngx-host.dll" "$stage/LICENSE" "$stage/NOTICE" "$stage/THIRD_PARTY.md" "$stage/OptiScaler/plugins/"*.log "$stage/"*.log "$out"/bench-* "$out/occ_ab" "$out/occ_nopad" "$out/occ_pad16k" "$out/b2_base" "$out/b2_post_exp" "$out/b2_post_log" "$out/b2_attr_rcp" "$out/b2_attr_same" "$out/b2_c64_far3"
mkdir -p -- "$out" "$stage/OptiScaler/plugins" "$stage/openrr-amd/shaders" "$stage/openrr-amd/shaders-gfx11"

jobs=${OPENRR_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}
if ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "invalid OPENRR_BUILD_JOBS: $jobs" >&2
    exit 1
fi
export OPENRR_BUILD_JOBS="$jobs"

cxx=${OPENRR_MINGW:-x86_64-w64-mingw32-g++}
cc=${OPENRR_MINGW_CC:-x86_64-w64-mingw32-gcc}
objdump=${OPENRR_OBJDUMP:-x86_64-w64-mingw32-objdump}
objcopy=${OPENRR_OBJCOPY:-x86_64-w64-mingw32-objcopy}
strip=${OPENRR_STRIP:-x86_64-w64-mingw32-strip}
windres=${OPENRR_WINDRES:-x86_64-w64-mingw32-windres}
glslang=${OPENRR_GLSLANG:-$(command -v glslangValidator || command -v glslang)}

command -v "$cxx" >/dev/null || { echo "missing mingw cross compiler ($cxx)" >&2; exit 1; }
[[ -n "$glslang" ]] || { echo "missing glslangValidator / glslang" >&2; exit 1; }

resolve_git_stamp() {
    local s
    if s=$(git rev-parse --short HEAD 2>/dev/null) && [[ -n "$s" ]]; then
        printf "%s" "$s"
        return
    fi
    if [[ -f .git ]] && command -v wslpath >/dev/null 2>&1; then
        local win_gitdir wsl_gitdir commondir
        win_gitdir=$(sed -n 's/^gitdir:[[:space:]]*//p' .git | tr -d '\r')
        if [[ -n "$win_gitdir" ]] && wsl_gitdir=$(wslpath -u "$win_gitdir" 2>/dev/null) && [[ -d "$wsl_gitdir" ]]; then
            commondir="$wsl_gitdir"
            if [[ -f "$wsl_gitdir/commondir" ]]; then
                commondir=$(realpath -m -- "$wsl_gitdir/$(tr -d '\r\n' < "$wsl_gitdir/commondir")")
            fi
            if s=$(GIT_DIR="$wsl_gitdir" GIT_COMMON_DIR="$commondir" git rev-parse --short HEAD 2>/dev/null) && [[ -n "$s" ]]; then
                printf "%s" "$s"
                return
            fi
        fi
    fi
    printf "dev"
}
stamp=${OPENRR_STAMP:-$(resolve_git_stamp)}

# Bounded worker pool: never exceeds $jobs processes, reaps every child cleanly on error or signal.
pool_pids=()
pool_failed=0

pool_cleanup() {
    local p
    for p in "${pool_pids[@]}"; do
        kill "$p" 2>/dev/null || true
    done
    wait "${pool_pids[@]}" 2>/dev/null || true
}
trap pool_cleanup INT TERM

pool_reap_one() {
    local rc=0 done_pid=""
    if (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then
        wait -n -p done_pid -- "${pool_pids[@]}" || rc=$?
        local next=() p
        for p in "${pool_pids[@]}"; do
            [[ "$p" == "$done_pid" ]] || next+=("$p")
        done
        pool_pids=(${next[@]+"${next[@]}"})
    else
        wait "${pool_pids[0]}" || rc=$?
        unset 'pool_pids[0]'
        pool_pids=(${pool_pids[@]+"${pool_pids[@]}"})
    fi
    (( rc == 0 )) || pool_failed=1
}

pool_run() {
    (( pool_failed == 0 )) || return 0
    while (( ${#pool_pids[@]} >= jobs )); do
        pool_reap_one
        (( pool_failed == 0 )) || return 0
    done
    "$@" &
    pool_pids+=($!)
}

pool_wait() {
    while (( ${#pool_pids[@]} > 0 )); do
        pool_reap_one
    done
    if (( pool_failed != 0 )); then
        pool_failed=0
        return 1
    fi
    return 0
}

minhook="asi/third_party/minhook"
vk_inc="asi/third_party/vulkan/include"

common_cxx=(-std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG
            -DOPENRR_BUILD_STAMP="\"$stamp\""
            -DONR_LT_DEEP="\"2,4,2,4\""
            -DONR_RR_BUILDING -DONR_RR_STATIC
            -I"$vk_inc" -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src
            -Iasi/src -I"$minhook/include" -I"$out")

echo "[1/4] Compiling GPU & Runtime sources..."
echo "[2/4] Compiling ASI shell, OptiScaler plugin, MinHook & tools (jobs=$jobs)..."
# Longest-job-first: heavy ASI/test/runtime translation units launch immediately so no core idles on a late tail.
pool_run "$cxx" "${common_cxx[@]}" -DOPENRR_TEST_HOOKS=1 -c asi/tests/test_asi_units.cpp -o "$out/test_asi_units.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_asi_main.cpp -o "$out/rr_asi_main.o"
pool_run "$cxx" "${common_cxx[@]}" -c runtime/src/runtime.cpp -o "$out/rt_runtime.o"
pool_run "$cxx" "${common_cxx[@]}" -DOPENRR_TEST_HOOKS=1 -c runtime/src/runtime.cpp -o "$out/rt_runtime_test.o"
pool_run "$cxx" "${common_cxx[@]}" -c gpu/src/vk.cpp -o "$out/gpu_vk.o"
pool_run "$cxx" "${common_cxx[@]}" -DOPENRR_TEST_HOOKS=1 -c gpu/src/vk.cpp -o "$out/gpu_vk_test.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_d3d12split.cpp -o "$out/rr_pe_d3d12split.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_opti_plugin.cpp -o "$out/rr_opti_plugin.o"
pool_run "$cxx" "${common_cxx[@]}" -c gpu/src/d3d12.cpp -o "$out/gpu_d3d12.o"
pool_run "$cxx" "${common_cxx[@]}" -DOPENRR_TEST_HOOKS=1 -c gpu/src/d3d12.cpp -o "$out/gpu_d3d12_test.o"
pool_run "$cxx" "${common_cxx[@]}" -c gpu/src/engine.cpp -o "$out/gpu_engine.o"
pool_run "$cxx" -std=c++20 -O2 -DNDEBUG -Iruntime/include -c runtime/bench/openrr_bench.cpp -o "$out/openrr_bench.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_log.cpp -o "$out/rr_pe_log.o"
pool_run "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hook.c" -o "$out/mh_hook.o"
pool_run "$cxx" "${common_cxx[@]}" -c runtime/src/vk_host.cpp -o "$out/rt_vk_host.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_config.cpp -o "$out/rr_pe_config.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_control.cpp -o "$out/rr_pe_control.o"
pool_run "$cxx" "${common_cxx[@]}" -c gpu/src/weights.cpp -o "$out/gpu_weights.o"
pool_run "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/trampoline.c" -o "$out/mh_trampoline.o"
pool_run "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/buffer.c" -o "$out/mh_buffer.o"
pool_run "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/hde64.c" -o "$out/mh_hde.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_crash.cpp -o "$out/rr_pe_crash.o"
pool_run "$cxx" "${common_cxx[@]}" -c asi/src/rr_pe_vkdevice.cpp -o "$out/rr_pe_vkdevice.o"
pool_run "$cxx" "${common_cxx[@]}" -c gpu/src/kernels.cpp -o "$out/gpu_kernels.o"
pool_run "$windres" -Iruntime/include -O coff asi/src/version.rc -o "$out/version_rc.o"
pool_wait

objs=("$out/rr_asi_main.o" "$out/rr_opti_plugin.o" "$out/rr_pe_control.o" "$out/rr_pe_config.o"
      "$out/rr_pe_vkdevice.o" "$out/rr_pe_d3d12split.o" "$out/rr_pe_log.o" "$out/rr_pe_crash.o"
      "$out/rt_runtime.o" "$out/rt_vk_host.o"
      "$out/gpu_vk.o" "$out/gpu_d3d12.o" "$out/gpu_kernels.o" "$out/gpu_weights.o" "$out/gpu_engine.o"
      "$out"/mh_*.o)

ldflags=(-ld3d12 -ldxgi -lole32 -lbcrypt -lversion -static -static-libgcc -static-libstdc++)

cat > "$out/openrr.def" << 'EOF'
LIBRARY "openrr-amd.asi"
EXPORTS
    InitializeASI
    OptiPluginInit
    OnrRrGetApi
    OnrRrSetFastMode
    OnrRrGetFastMode
    OnrRrSetFp8
    OnrRrGetFp8
    OnrRrNotifyD3d12ListReset
    OnrRrNotifyD3d12ListReleased
    OnrRrNotifyD3d12AllocatorReset
    OnrRrNotifyD3d12AllocatorReleased
    OnrRrNotifyVkCommandBuffersAllocated
    OnrRrNotifyVkCommandBufferReset
    OnrRrNotifyVkCommandBuffersFreed
    OnrRrNotifyVkCommandPoolReset
    OnrRrNotifyVkCommandPoolDestroyed
    OnrRrNotifyVkDeviceDestroyed
    OpenRR_IsInDirectRecord
    rr_vk_load
EOF

echo "[3/4] Generating Vulkan lazy loader & linking openrr-amd.asi..."
count=$(bash asi/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}" | grep -o '[0-9]*$')

link_and_strip_asi() {
    "$cxx" -shared -o "$stage/OptiScaler/plugins/openrr-amd.asi" "$out/openrr.def" "${objs[@]}" "$out/version_rc.o" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"
    rm -f "$out/openrr-amd.debug"
    "$objcopy" --only-keep-debug "$stage/OptiScaler/plugins/openrr-amd.asi" "$out/openrr-amd.debug"
    "$strip" --strip-unneeded "$stage/OptiScaler/plugins/openrr-amd.asi"
    "$objcopy" --add-gnu-debuglink="$out/openrr-amd.debug" "$stage/OptiScaler/plugins/openrr-amd.asi"
}

test_objs=("$out/test_asi_units.o"
           "$out/rr_pe_config.o" "$out/rr_pe_d3d12split.o" "$out/rr_pe_vkdevice.o" "$out/rr_pe_log.o" "$out/rr_pe_crash.o"
           "$out/rt_runtime_test.o" "$out/rt_vk_host.o" "$out/gpu_vk_test.o" "$out/gpu_d3d12_test.o" "$out/gpu_kernels.o"
           "$out/gpu_weights.o" "$out/gpu_engine.o" "$out"/mh_*.o)

link_test_units() {
    mkdir -p -- "$out/test_vk_lazy"
    bash asi/build/vulkan_lazy.sh "$out/test_vk_lazy" "$cxx" -std=c++20 -O2 "${test_objs[@]}" "${ldflags[@]}" >/dev/null
    "$cxx" -std=c++20 -O2 "${test_objs[@]}" "$out/test_vk_lazy/vulkan_lazy_s.o" "$out/test_vk_lazy/vulkan_lazy_c.o" "${ldflags[@]}" -o "$out/test_asi_units.exe"
}

echo "[4/4] Linking binaries, building runtime SPIR-V shaders & staging package..."
mkdir -p -- "$stage/openrr-amd/lang/fonts"
pool_run link_and_strip_asi
pool_run link_test_units
pool_run "$cxx" -std=c++20 -O2 "$out/openrr_bench.o" \
    -ld3d12 -ldxgi -lole32 -lshell32 -static -static-libgcc -static-libstdc++ -o "$out/openrr_bench.exe"
pool_run python3 asi/build/make_lang.py "$stage/openrr-amd/lang/English.txt" asi/src/rr_opti_plugin.cpp
pool_wait

python3 asi/build/build_shaders.py "$glslang" gpu "$stage/openrr-amd/shaders" --jobs "$jobs"
python3 asi/build/build_shaders.py "$glslang" gpu "$stage/openrr-amd/shaders-gfx11" --gfx11 --jobs "$jobs"
cp asi/openrr-amd.ini "$stage/openrr-amd.ini"
cp -- asi/lang/zh-CN.txt asi/lang/ja.txt asi/lang/ko.txt "$stage/openrr-amd/lang/"
cp -- asi/lang/fonts/README.txt asi/lang/fonts/Noto-CJK-LICENSE.txt "$stage/openrr-amd/lang/fonts/"
cjk_font="${OPENRR_CJK_FONT_DIR:-/mnt/c/Users/Kenny/Downloads/DLSSNR-AMD-lang-CJK-0.0.9/lang/fonts}/NotoSansCJKsc-Regular.ttf"
if [[ -f "asi/lang/fonts/NotoSansCJKsc-Regular.ttf" ]]; then
    cp -- "asi/lang/fonts/NotoSansCJKsc-Regular.ttf" "$stage/openrr-amd/lang/fonts/"
elif [[ -f "$cjk_font" ]]; then
    cp -- "$cjk_font" "$stage/openrr-amd/lang/fonts/"
fi
cp -- LICENSE "$stage/openrr-amd/OpenRR_LICENSE.txt"
cp -- NOTICE "$stage/openrr-amd/OpenRR_NOTICE.txt"
cp -- THIRD_PARTY.md "$stage/openrr-amd/OpenRR_THIRD_PARTY.md"

# Offline ACO packaging. Native Mesa stays outside the Windows package.
if [[ "${OPENRR_BUILD_ACO:-1}" == "1" ]]; then
    bash asi/build/mesa/build_capture_263.sh
    tools=build/mesa-comparison/mesa-263-drirc-toolchain
    aco_common_args=(
        --toolchain "$tools/toolchain.json" --capture "$tools/aco_capture"
        --out "$stage/openrr-amd/aco"
        --precision "${OPENRR_ACO_PRECISION:-global}" --precision-split --precision-native-only post_f_lt d3d_enc0_attr_f_lt d3d_enc0_attr_f_w_lt enc0_attr_f_lt enc0_attr_f_w_lt post_dlt_f_lt dec_head_32_64_f_lt dec_head_32_64_f_lx enc0_stage_32_64_f_lt enc0_stage_32_64_f_lx enc0_stage_32_64_f_w_lt enc0_stage_32_64_f_w_lx dec_up_32_64_64_f_lt dec_up_32_64_64_f_lx enc_stage_64_64_96_f_lt dec_up_64_64_96_f_lt dec_dfn_64_64_f_lt enc_stage_96_128_128_f_lt enc_stage_96_128_128_fl_lt dec_up_96_128_128_f_lt dec_up_96_128_128_fl_lt dec_dfn_96_128_f_lt dec_dfn_96_128_fl_lt enc_stage_128_128_160_f_lt enc_stage_128_128_160_fl_lt dec_up_128_128_160_fl_lt dec_up_128_128_160_f_lt dec_dfn_128_128_f_lt dec_dfn_128_128_fl_lt dec_dfn_160_256_fl_lt dec_dfn_160_256_f_lt enc_stage_160_256_0_fl_lt enc_stage_160_256_0_f_lt enc_stage_160_256_160_fl_lt enc_stage_160_256_160_f_lt dec_up_160_256_160_f_lt dec_up_160_256_160_fl_lt --matrix-loads "${OPENRR_ACO_MATRIX_LOADS:-vector8}" --shared-cmat "${OPENRR_ACO_SHARED_CMAT:-off}"
        "$stage/openrr-amd/shaders"
        --jobs "$jobs"
        --cmat-address-shaders dec_dfn_128_128_f dec_dfn_128_128_f_lt dec_dfn_128_128_fl dec_dfn_128_128_fl_lt dec_dfn_160_256_f dec_dfn_160_256_f_lt dec_dfn_160_256_fl dec_dfn_160_256_fl_lt dec_dfn_64_64_f_lt dec_dfn_96_128_f dec_dfn_96_128_f_lt dec_dfn_96_128_fl dec_dfn_96_128_fl_lt dec_head_32_64_f dec_head_32_64_f_lt dec_head_32_64_f_lx dec_up_128_128_160_f dec_up_128_128_160_f_lt dec_up_128_128_160_fl dec_up_128_128_160_fl_lt dec_up_160_256_160_f dec_up_160_256_160_f_lt dec_up_160_256_160_fl dec_up_160_256_160_fl_lt dec_up_32_64_64_f dec_up_32_64_64_f_lt dec_up_32_64_64_f_lx dec_up_64_64_96_f_lt dec_up_96_128_128_f dec_up_96_128_128_f_lt dec_up_96_128_128_fl dec_up_96_128_128_fl_lt enc0_stage_32_64_f enc0_stage_32_64_f_lt enc0_stage_32_64_f_lx enc0_stage_32_64_f_w enc0_stage_32_64_f_w_lt enc0_stage_32_64_f_w_lx enc_stage_128_128_160_f enc_stage_128_128_160_f_lt enc_stage_128_128_160_fl enc_stage_128_128_160_fl_lt enc_stage_160_256_0_f enc_stage_160_256_0_f_lt enc_stage_160_256_0_fl enc_stage_160_256_0_fl_lt enc_stage_160_256_160_f enc_stage_160_256_160_f_lt enc_stage_160_256_160_fl enc_stage_160_256_160_fl_lt enc_stage_64_64_96_f enc_stage_64_64_96_f_lt enc_stage_96_128_128_f enc_stage_96_128_128_f_lt enc_stage_96_128_128_fl enc_stage_96_128_128_fl_lt
    )
    for aco_target in gfx1201 gfx1200; do
        DRIRC_CONFIGDIR="$PWD/build/mesa-comparison/mesa-263-drirc/drirc" \
        python3 asi/build/build_aco_records.py \
            --target "$aco_target" --capture-dir "$out/aco-capture-$aco_target" \
            "${aco_common_args[@]}"
    done
fi
echo "Built $stage/OptiScaler/plugins/openrr-amd.asi ($(stat -c %s "$stage/OptiScaler/plugins/openrr-amd.asi") bytes, $count Vulkan entry points resolved lazily)"
export_count=$("$objdump" -p "$stage/OptiScaler/plugins/openrr-amd.asi" | grep -Ec '[[:space:]](InitializeASI|OptiPluginInit)$')
echo "Exports: $export_count  Imports: $("$objdump" -p "$stage/OptiScaler/plugins/openrr-amd.asi" | grep 'DLL Name' | awk '{print $3}' | tr '\n' ' ')"
if [[ "$export_count" -ne 2 ]]; then
    echo "FATAL: expected both InitializeASI and OptiPluginInit exports in openrr-amd.asi (found $export_count)" >&2
    exit 1
fi
pkg_zip=${OPENRR_PACKAGE_ZIP:-auto}
if [[ "$pkg_zip" == "1" || ( "$pkg_zip" == "auto" && -f "$stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.json" && -f "$stage/openrr-amd/aco/gfx1200/openrr-d3d12-v1/manifest.json" ) ]]; then
    pkg_args=(--stage "$stage" --out-dir "${OPENRR_DIST_DIR:-dist}" --jobs "$jobs")
    if [[ -n "${OPENRR_PACKAGE_NAME:-}" ]]; then
        pkg_args+=(--name "$OPENRR_PACKAGE_NAME")
    fi
    python3 asi/build/package_release.py "${pkg_args[@]}"
fi
