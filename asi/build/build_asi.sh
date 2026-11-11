#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds standalone openrr-amd.asi (OptiScaler plugin with embedded OpenRR runtime + GPU engine) via MinGW-w64.
# Usage: bash asi/build/build_asi.sh [build_dir]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=$(realpath -m -- "${1:-build/asi}")
stage="$out/stage"
rm -rf -- "$out"/bench-* "$out/occ_ab"
mkdir -p -- "$out" "$stage/plugins" "$stage/openrr-amd/shaders"

cxx=${OPENRR_MINGW:-x86_64-w64-mingw32-g++}
cc=${OPENRR_MINGW_CC:-x86_64-w64-mingw32-gcc}
objdump=${OPENRR_OBJDUMP:-x86_64-w64-mingw32-objdump}
objcopy=${OPENRR_OBJCOPY:-x86_64-w64-mingw32-objcopy}
strip=${OPENRR_STRIP:-x86_64-w64-mingw32-strip}
glslang=${OPENRR_GLSLANG:-$(command -v glslangValidator || command -v glslang)}

command -v "$cxx" >/dev/null || { echo "missing mingw cross compiler ($cxx)" >&2; exit 1; }
[[ -n "$glslang" ]] || { echo "missing glslangValidator / glslang" >&2; exit 1; }

stamp=${OPENRR_STAMP:-$(git rev-parse --short HEAD 2>/dev/null || echo "dev")}

minhook="asi/third_party/minhook"
imgui="asi/third_party/imgui"
vk_inc="asi/third_party/vulkan/include"
font="asi/third_party/inter/Inter-Regular.ttf"

python3 asi/build/embed_font.py "$font" "$out/rr_menu_font.hpp" kMenuFont
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeVert asi/shaders/menu_compose.vert -o "$out/menu_compose_vert.h" > /dev/null
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeFrag asi/shaders/menu_compose.frag -o "$out/menu_compose_frag.h" > /dev/null

common_cxx=(-std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG
            -DOPENRR_BUILD_STAMP="\"$stamp\""
            -DONR_RR_BUILDING -DONR_RR_STATIC -DOPENRR_EXPERIMENTAL_OPTIFG_HOST
            -I"$vk_inc" -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src
            -Iasi/src -I"$minhook/include" -I"$imgui" -I"$out")

echo "[1/4] Compiling GPU & Runtime sources..."
pids=()
for unit in vk d3d12 kernels weights engine; do
    "$cxx" "${common_cxx[@]}" -c "gpu/src/$unit.cpp" -o "$out/gpu_$unit.o" &
    pids+=($!)
done
for unit in runtime vk_host; do
    "$cxx" "${common_cxx[@]}" -c "runtime/src/$unit.cpp" -o "$out/rt_$unit.o" &
    pids+=($!)
done

echo "[2/4] Compiling ASI shell, ImGui & MinHook..."
for unit in rr_pe_log rr_pe_crash rr_pe_d3d12split rr_pe_vkdevice rr_pe_config rr_pe_control rr_pe_menu rr_asi_main; do
    "$cxx" "${common_cxx[@]}" -c "asi/src/$unit.cpp" -o "$out/$unit.o" &
    pids+=($!)
done
for unit in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx11 imgui_impl_vulkan; do
    "$cxx" -std=c++17 -O2 -DNDEBUG -I"$vk_inc" -I"$imgui" -c "$imgui/$unit.cpp" -o "$out/im_$unit.o" &
    pids+=($!)
done
"$cxx" -std=c++17 -O2 -DNDEBUG -I"$imgui" -include asi/src/mingw_d3d12_compat.h -c "$imgui/imgui_impl_dx12.cpp" -o "$out/im_imgui_impl_dx12.o" &
pids+=($!)
for unit in hook buffer trampoline; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o" &
    pids+=($!)
done
"$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/hde64.c" -o "$out/mh_hde.o" &
pids+=($!)
for pid in "${pids[@]}"; do
    wait "$pid"
done

objs=("$out/rr_asi_main.o" "$out/rr_pe_menu.o" "$out/rr_pe_control.o" "$out/rr_pe_config.o"
      "$out/rr_pe_vkdevice.o" "$out/rr_pe_d3d12split.o" "$out/rr_pe_log.o" "$out/rr_pe_crash.o"
      "$out/rt_runtime.o" "$out/rt_vk_host.o"
      "$out/gpu_vk.o" "$out/gpu_d3d12.o" "$out/gpu_kernels.o" "$out/gpu_weights.o" "$out/gpu_engine.o"
      "$out"/im_*.o "$out"/mh_*.o)

ldflags=(-ld3d12 -ld3d11 -ldxgi -lole32 -lbcrypt -lversion -static -static-libgcc -static-libstdc++)

cat > "$out/openrr.def" << 'EOF'
LIBRARY "openrr-amd.asi"
EXPORTS
    InitializeASI
    OnrRrGetApi
    OnrRrSetFastMode
    OnrRrGetFastMode
    OnrRrNotifyD3d12ListReset
    OnrRrNotifyD3d12ListReleased
    OnrRrNotifyD3d12AllocatorReset
    OnrRrNotifyD3d12AllocatorReleased
    rr_vk_load
EOF

echo "[3/4] Generating Vulkan lazy loader & linking openrr-amd.asi..."
"$cxx" -std=c++20 -O2 -Iasi/src asi/tests/test_present_scope.cpp -static -o "$out/test_present_scope.exe"
"$cxx" -std=c++20 -O2 -Iasi/src -I"$minhook/include" asi/tests/test_dxgi_queue.cpp \
    "$out"/mh_*.o -ld3d12 -ldxgi -lole32 -static -static-libgcc -static-libstdc++ -o "$out/test_dxgi_queue.exe"
count=$(bash asi/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}" | grep -o '[0-9]*$')
"$cxx" -shared -o "$stage/plugins/openrr-amd.asi" "$out/openrr.def" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"
rm -f "$out/openrr-amd.debug"
"$objcopy" --only-keep-debug "$stage/plugins/openrr-amd.asi" "$out/openrr-amd.debug"
"$strip" --strip-unneeded "$stage/plugins/openrr-amd.asi"
"$objcopy" --add-gnu-debuglink="$out/openrr-amd.debug" "$stage/plugins/openrr-amd.asi"

echo "[4/4] Building runtime SPIR-V shaders & staging package..."
python3 asi/build/build_shaders.py "$glslang" gpu "$stage/openrr-amd/shaders"
cp asi/openrr-amd.ini "$stage/openrr-amd.ini"
"$cxx" -std=c++17 -O2 -shared -static -Iasi/src asi/src/optifg_ngx_proxy.cpp -o "$stage/openrr-ngx-host.dll"
mkdir -p -- "$out/occ_nopad" "$out/occ_pad16k"
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=2 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/enc0_stage_32_64.comp -o "$out/occ_nopad/enc0_stage_32_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_POST_PAIR=1 -DLT_POST_EXP=1 -DLT_POST_NOCORNER=1 -DLT_POST_NETH=1 -Igpu/shaders/include gpu/shaders/post.comp -o "$out/occ_nopad/post_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_up_32_64_64.comp -o "$out/occ_nopad/dec_up_32_64_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_head_32_64.comp -o "$out/occ_nopad/dec_head_32_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/enc_stage_64_64_96.comp -o "$out/occ_nopad/enc_stage_64_64_96_f_lt.spv" > /dev/null

"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=2 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -DLDS_OCC_PAD=8192 -Igpu/shaders/include gpu/shaders/enc0_stage_32_64.comp -o "$out/occ_pad16k/enc0_stage_32_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_POST_PAIR=1 -DLT_POST_EXP=1 -DLT_POST_NOCORNER=1 -DLT_POST_NETH=1 -DPOST_OCC_PAD=4096 -Igpu/shaders/include gpu/shaders/post.comp -o "$out/occ_pad16k/post_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -DLDS_OCC_PAD=8192 -Igpu/shaders/include gpu/shaders/dec_up_32_64_64.comp -o "$out/occ_pad16k/dec_up_32_64_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_Q16=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -DLDS_OCC_PAD=8192 -Igpu/shaders/include gpu/shaders/dec_head_32_64.comp -o "$out/occ_pad16k/dec_head_32_64_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -DLDS_OCC_PAD=8192 -Igpu/shaders/include gpu/shaders/enc_stage_64_64_96.comp -o "$out/occ_pad16k/enc_stage_64_64_96_f_lt.spv" > /dev/null

mkdir -p -- "$out/b2_base" "$out/b2_post_exp" "$out/b2_post_log" "$out/b2_attr_rcp" "$out/b2_attr_same" "$out/b2_c64_far3"
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_POST_PAIR=1 -Igpu/shaders/include gpu/shaders/post.comp -o "$out/b2_base/post_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_ATTR_TAB=1 -Igpu/shaders/include gpu/shaders/enc0_attr.comp -o "$out/b2_base/enc0_attr_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/enc_stage_64_64_96.comp -o "$out/b2_base/enc_stage_64_64_96_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_up_64_64_96.comp -o "$out/b2_base/dec_up_64_64_96_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_dfn_64_64.comp -o "$out/b2_base/dec_dfn_64_64_f_lt.spv" > /dev/null

"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_POST_PAIR=1 -DLT_POST_EXP=1 -DLT_POST_NOCORNER=1 -DLT_POST_NETH=1 -Igpu/shaders/include gpu/shaders/post.comp -o "$out/b2_post_exp/post_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_POST_PAIR=1 -DLT_POST_EXP=1 -DLT_POST_NOCORNER=1 -DLT_POST_NETH=1 -DLT_POST_LOG=1 -Igpu/shaders/include gpu/shaders/post.comp -o "$out/b2_post_log/post_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_ATTR_TAB=1 -DLT_ATTR_RCP=1 -DLT_ATTR_DEPTH=1 -Igpu/shaders/include gpu/shaders/enc0_attr.comp -o "$out/b2_attr_rcp/enc0_attr_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DD3D_GUIDES=1 -DENC_F32=1 -DLT_ATTR_TAB=1 -DLT_ATTR_RCP=1 -DLT_ATTR_DEPTH=1 -Igpu/shaders/include gpu/shaders/enc0_attr.comp -o "$out/b2_attr_rcp/d3d_enc0_attr_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_ATTR_TAB=1 -DLT_ATTR_RCP=1 -DLT_ATTR_DEPTH=1 -DLT_ATTR_SAME=1 -DLT_ATTR_CL=1 -Igpu/shaders/include gpu/shaders/enc0_attr.comp -o "$out/b2_attr_same/enc0_attr_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/enc_stage_64_64_96.comp -o "$out/b2_c64_far3/enc_stage_64_64_96_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_up_64_64_96.comp -o "$out/b2_c64_far3/dec_up_64_64_96_f_lt.spv" > /dev/null
"$glslang" -V --target-env vulkan1.3 -DENC_F32=1 -DLT_FAR=3 -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -Igpu/shaders/include gpu/shaders/dec_dfn_64_64.comp -o "$out/b2_c64_far3/dec_dfn_64_64_f_lt.spv" > /dev/null
"$cxx" "${common_cxx[@]}" asi/tests/test_asi_units.cpp \
    "$out/rr_pe_config.o" "$out/rr_pe_d3d12split.o" "$out/rr_pe_vkdevice.o" "$out/rr_pe_log.o" "$out/rr_pe_crash.o" \
    "$out/rt_runtime.o" "$out/rt_vk_host.o" "$out/gpu_vk.o" "$out/gpu_d3d12.o" "$out/gpu_kernels.o" \
    "$out/gpu_weights.o" "$out/gpu_engine.o" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" \
    "$out"/mh_*.o "${ldflags[@]}" -o "$out/test_asi_units.exe"
"$cxx" -std=c++20 -O2 -DNDEBUG -Iruntime/include runtime/bench/openrr_bench.cpp \
    -ld3d12 -ldxgi -lole32 -lshell32 -static -static-libgcc -static-libstdc++ -o "$out/openrr_bench.exe"

echo "Built $stage/plugins/openrr-amd.asi ($(stat -c %s "$stage/plugins/openrr-amd.asi") bytes, $count Vulkan entry points resolved lazily)"
echo "Exports: $("$objdump" -p "$stage/plugins/openrr-amd.asi" | grep -c 'InitializeASI')  Imports: $("$objdump" -p "$stage/plugins/openrr-amd.asi" | grep 'DLL Name' | awk '{print $3}' | tr '\n' ' ')"
