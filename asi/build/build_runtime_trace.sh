#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# Reuse existing CPU objects; leave archived ASIs and staged package untouched.
base=${OPENRR_TRACE_BASE:-build/asi}
out=${OPENRR_TRACE_OUT:-build/aco-runtime-trace}
mkdir -p "$out"
cxx=x86_64-w64-mingw32-g++
flags=(-std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG -DONR_RR_STATIC -DONR_RR_BUILDING -DOPENRR_EXPERIMENTAL_OPTIFG_HOST
  -Iasi/third_party/vulkan/include -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src
  -Iasi/src -Iasi/third_party/minhook/include -I"$base")
# A D3dDevice member was added, so rebuild every object which instantiates it.
for unit in vk d3d12 kernels weights engine; do
  "$cxx" "${flags[@]}" -c "gpu/src/$unit.cpp" -o "$out/gpu_$unit.o"
done
"$cxx" "${flags[@]}" -c runtime/src/runtime.cpp -o "$out/rt_runtime.o"
cp "$base/openrr.def" "$out/openrr.def"
printf '\n    OnrRrDiagnostic\n' >> "$out/openrr.def"
"$cxx" -shared -o "$out/openrr-trace.asi" "$out/openrr.def" \
  "$base/rr_asi_main.o" "$base/rr_pe_menu.o" "$base/rr_pe_control.o" "$base/rr_pe_config.o" \
  "$base/rr_pe_vkdevice.o" "$base/rr_pe_d3d12split.o" "$base/rr_pe_log.o" "$base/rr_pe_crash.o" \
  "$out/rt_runtime.o" "$base/rt_vk_host.o" "$out"/gpu_*.o "$base"/im_*.o "$base"/mh_*.o \
  "$base/vulkan_lazy_s.o" "$base/vulkan_lazy_c.o" \
  -ld3d12 -ld3d11 -ldxgi -lole32 -lbcrypt -lversion -static -static-libgcc -static-libstdc++
"$cxx" -std=c++20 -O2 -DNDEBUG -Iruntime/include runtime/bench/openrr_bench.cpp \
  -ld3d12 -ldxgi -lole32 -lshell32 -static -static-libgcc -static-libstdc++ -o "$out/openrr_bench.exe"

# Offline OpenRR-vs-RR_Contrast parity evaluator (anchor #2); built when the read-only
# RR_Contrast checkout is reachable so the fixture synthesis is included verbatim.
rr_inc=${RR_CONTRAST_INC:-/mnt/c/Users/Kenny/Downloads/OpenRR/build/RR_Contrast/include}
if [[ -f "$rr_inc/rr_ref_scenes.hpp" ]]; then
  "$cxx" -std=c++20 -O2 -DNDEBUG -Iruntime/include -I"$rr_inc" runtime/bench/openrr_contrast_eval.cpp \
    -ld3d12 -ldxgi -lole32 -lshell32 -static -static-libgcc -static-libstdc++ -o "$out/openrr_contrast_eval.exe"
  echo "built $out/openrr_contrast_eval.exe"
fi
# The speed gates load the runtime from build/aco-v015; keep it in lockstep with this build.
if [[ "${OPENRR_TRACE_BASE:-build/asi}" == "build/asi-015-aco" ]]; then
  mkdir -p build/aco-v015
  cp -f "$out/openrr-trace.asi" build/aco-v015/openrr-trace.asi
  cp -f "$out/openrr_bench.exe" build/aco-v015/openrr_bench.exe
  cp -f "$out/openrr_contrast_eval.exe" build/aco-v015/openrr_contrast_eval.exe 2>/dev/null || true
  # The units gate is build_asi.sh's own test_asi_units.exe (same sources, native
  # stage/occ_* tree resolution); a trace-side copy is redundant and was removed.
  echo "synced build/aco-v015 runtime"
fi
