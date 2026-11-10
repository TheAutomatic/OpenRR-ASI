#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=build/asi
cxx=x86_64-w64-mingw32-g++
"$cxx" -std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG -DONR_RR_STATIC -DONR_RR_BUILDING \
    -Iasi/third_party/vulkan/include -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src \
    -Iasi/src -Iasi/third_party/minhook/include -I"$out" -c runtime/src/runtime.cpp -o "$out/rt_runtime.o"
"$cxx" -std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG -DONR_RR_STATIC -DONR_RR_BUILDING \
    -Iasi/third_party/vulkan/include -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src \
    -Iasi/src -Iasi/third_party/minhook/include -I"$out" -c gpu/src/d3d12.cpp -o "$out/gpu_d3d12.o"
"$cxx" -std=c++20 -mf16c -O2 -fno-strict-aliasing -DNDEBUG -DONR_RR_STATIC -DONR_RR_BUILDING \
    -Iasi/third_party/vulkan/include -Igpu/include -Igpu/src -Iruntime/include -Iruntime/src \
    -Iasi/src -Iasi/third_party/minhook/include -I"$out" asi/tests/test_asi_units.cpp \
    "$out/rr_pe_config.o" "$out/rr_pe_d3d12split.o" "$out/rr_pe_vkdevice.o" "$out/rr_pe_log.o" "$out/rr_pe_crash.o" \
    "$out/rt_runtime.o" "$out/rt_vk_host.o" "$out"/gpu_*.o "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" \
    "$out"/mh_*.o -ld3d12 -ld3d11 -ldxgi -lole32 -lbcrypt -lversion -static -static-libgcc -static-libstdc++ \
    -o "$out/test_aco_units.exe"
