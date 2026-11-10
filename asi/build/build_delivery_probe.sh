#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build-only ABI fixtures; never add them to a package stage.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
build=${1:-build/asi-delivery-candidate}
out=${2:-build/aco-delivery-review}
mkdir -p "$out/probe-data/shaders"
glslangValidator -V --target-env vulkan1.3 -Igpu/shaders/include \
    asi/tests/aco_abi.comp -o "$out/probe-data/shaders/aco_abi.spv"
for name in selftest selftest_tex clear_range; do
    cp "$build/stage/openrr-amd/shaders/$name.spv" "$out/probe-data/shaders/"
done
python3 asi/build/build_aco_records.py "$out/probe-data/shaders" \
    --out "$out/probe-data/aco" --capture-dir "$out/probe-capture" \
    --precision global --matrix-loads vector8
