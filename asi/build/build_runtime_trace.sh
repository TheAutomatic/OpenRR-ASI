#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# Reuse existing CPU objects; leave archived ASIs and staged package untouched.
base=${OPENRR_TRACE_BASE:-build/asi}
out=${OPENRR_TRACE_OUT:-build/aco-runtime-trace}
mkdir -p "$out"
cxx=x86_64-w64-mingw32-g++
flags=(-std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG -DONR_RR_STATIC -DONR_RR_BUILDING
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
"$cxx" "${flags[@]}" asi/tests/test_asi_units.cpp \
  "$base/rr_pe_config.o" "$base/rr_pe_d3d12split.o" "$base/rr_pe_vkdevice.o" "$base/rr_pe_log.o" "$base/rr_pe_crash.o" \
  "$out/rt_runtime.o" "$base/rt_vk_host.o" "$out"/gpu_*.o "$base/vulkan_lazy_s.o" "$base/vulkan_lazy_c.o" \
  "$base"/mh_*.o -ld3d12 -ld3d11 -ldxgi -lole32 -lbcrypt -lversion -static -static-libgcc -static-libstdc++ \
  -o "$out/test_aco_units.exe"
