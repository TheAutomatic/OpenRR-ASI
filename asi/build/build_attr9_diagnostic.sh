#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=build/aco-runtime-trace/attr9
base=build/asi
mkdir -p "$out"
cp "$base/stage/openrr-amd/shaders/clear_range.spv" "$out/clear_range.spv"
glslang=${GLSLANG:-/home/kenny/tools/glslang-build/StandAlone/glslang}
if [[ ! -x "$glslang" ]]; then glslang=$(command -v glslangValidator); fi
"$glslang" -V --target-env vulkan1.3 -Igpu/shaders/include asi/tests/aco_attr9.comp -o "$out/aco_attr9.spv"
x86_64-w64-mingw32-g++ -std=c++20 -mf16c -O2 -DNDEBUG \
 -Iasi/third_party/vulkan/include -Igpu/include -Igpu/src -Iasi/src \
 asi/tests/aco_attr9.cpp build/aco-runtime-trace/gpu_vk.o build/aco-runtime-trace/gpu_d3d12.o \
 "$base/vulkan_lazy_s.o" "$base/vulkan_lazy_c.o" \
 -ld3d12 -ldxgi -lole32 -static -static-libgcc -static-libstdc++ -o "$out/aco_attr9.exe"
python3 asi/build/build_aco_records.py "$out/aco_attr9.spv" --out "$out/records" --precision global --matrix-loads vector8
