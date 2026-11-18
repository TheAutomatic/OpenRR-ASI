#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
for policy in global-vector8 global-scalar local-vector8 local-scalar native-scalar; do
    precision=${policy%-*}
    loads=${policy#*-}
    python3 asi/build/build_aco_records.py --out "build/aco-review-v4/$policy" \
        --precision "$precision" --matrix-loads "$loads" \
        build/asi/stage/openrr-amd/shaders > "build/aco-tools/review-v4-$policy.log" 2>&1
done
