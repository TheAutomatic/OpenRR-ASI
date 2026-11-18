# ACO performance baseline and projection-prefetch experiment — 2026-11-10

Branch codex/mesa-aco; all changes remain uncommitted. Original staged shaders,
records and release package were not overwritten. The variant is rejected.
The current baseline remains global/vector8 with strict ACO on D3D12.

## Baseline after diagnostic repair

The runtime diagnostic baseline is established in
mesa-aco-runtime-diagnostic-results-2026-11-10.md. Legacy was exact across all
20 measured frames. Fast retains its existing nonzero differences; its saved
frame-6 first live nonzero divergence is explained by normalization/interpolation
evaluation order. Diagnostic repair does not mean Fast has become bit exact.
No arithmetic correction or precision-policy change was made to force equality.

The user authorized performance testing while the GPU was idle. Six paired
Windows D3D12/PAL runs used the same isolated diagnostic ASI, strict ACO for
both sides, global/vector8, flow/direct guides/direct output/sampler enabled,
1280x720 to 1920x1080, phase alignment, 24 warmup + 48 steady + 24 DRS frames.
Trace was disabled and per-frame correctness readbacks were disabled. Final
output readback remains outside the measured GPU region. Each process ran both
sides; original/variant order alternated, with three runs of each order.
Runtime path logs were checked in every run. WSL was used only for offline
compilation and CPU analysis, never for GPU timing.

Each table entry is the median of six per-run medians; the range is the minimum
and maximum per-run median. These are end-to-end runtime measurements.

| Configuration | Steady ms | Steady range ms | DRS ms | DRS range ms |
|---|---:|---:|---:|---:|
| Current ACO baseline | 3.70388 | 3.69124–3.72408 | 3.53114 | 3.51940–3.58296 |
| Reduced projection prefetch | 3.71204 | 3.70404–3.73592 | 3.56106 | 3.55344–3.61360 |

The variant increases the aggregate steady median by 0.22% and DRS median by
0.85%. Paired steady changes range from -0.30% to +1.19%; DRS from -0.68% to
+1.74%. This is not evidence of a reliable speedup. No fresh LLPC timing was
performed, so these measurements do not update the previously accepted 19%
ACO-versus-LLPC regression.

## Single-factor hypothesis and implementation

Kernel: dec_dfn_96_128_fl_lt, NT6 W2_DEFER output projection in
gpu/shaders/include/onr_block_w2.glsl. The isolated variant changes two live
weight pairs p0/p1/q0/q1 to one pair p0/p1, loading the next pair after its
predecessor is consumed. OLD addresses, tiled layout, and each accumulator's
MMA order are unchanged. No weights, reduction order or precision boundaries
are changed. Only this SPIR-V differs in the isolated shader directory.

Mochi's Linux ACO experiments document that deeper prefetch pipelines can
increase VGPR pressure without improving frame time. That observation motivated
this RR-specific live-range experiment; no Mochi code was copied. The shader
baseline was rebuilt and verified byte-identical before generating the variant.

| Resource / identity | Original | Variant |
|---|---|---|
| SPIR-V FNV | 9b09c8e4e1c7b892 | 19957c1b1dd28ad6 |
| ACO code FNV | b9e6894e6d159a22 | 82b3d7e38465f8dc |
| VGPR | 256 | 256 |
| SGPR | 108 | 108 |
| Scratch B/thread | 0 | 0 |
| LDS B/workgroup | 9728 | 9728 |
| Executable bytes | 27560 | 27604 |

Offline final hardware IR was captured for both and each code body verified
byte-identical to its experiment record. Load and MMA static instruction counts
are unchanged. Changed static counts are s_delay_alu 174→171, s_wait_loadcnt
92→91, s_waitcnt_depctr 444→449, v_mov_b32 25→26 and v_swap_b32 4→3. Static
counts are not executed instruction counts. The expected allocation reduction
did not occur; there is no demonstrated residency increase. No claim about
driver-final programmed register observation is made.

## Correctness before timing

The untraced control used 8 warmup + 8 steady + 12 DRS frames, explicit LLPC
against strict ACO. All existing Fast gates passed, with no non-finite outputs.
For each compiler, all 20 measured outputs were byte-identical to the original
untraced baseline. Thus this variant adds no measured output error.

Frame-6 trace was then checked against the variant's own untraced control: all
20 measured outputs match for each compiler. All 69 intermediate binary
readbacks per compiler also match the original frame-6 trace byte for byte.
The first live nonzero divergence remains dispatch 3 d3d_enc0_attr_f_lt,
attribute 9 at pixel (203,378), byte 23234111, LLPC 0x6e versus ACO 0x6f.
This includes entry guides/history snapshots; no later contaminated entry is
used as the source. Candidate warmup frames 0–5 were not independently retraced.

The structural comparator checked groups, indirect, non-address push fields,
projection/frame state and guide layouts. Its address-review limitation remains
explicit; this experiment does not introduce a new claim of automatic address
normalization. Legacy SPIR-V and records are unchanged; Legacy was not rerun
for this single Fast-only variant. Its earlier 20-frame exact control remains
the Legacy baseline.

## Decision and next evidence

Reject reduced projection prefetch: it preserves measured correctness but does
not reduce allocated registers or improve end-to-end time. Keep only isolated
source/records and evidence for review; do not copy it into shipping shaders.
No additional timing repetition is justified for this candidate.

Before another shader experiment, inspect the actual spilled
enc_stage_96_128_128_fl_lt live ranges and its W2 attention/projection/MLP
boundary. A next candidate must target an observed allocation/spill or address
dependency, state the unchanged numerical order, and pass the saved runtime
case before timing. Shrinking MLP slices or changing accumulation order is a
separate hypothesis and has not been implemented here. Native same-GPU RADV
execution remains unavailable in current WSL; the native Linux proposal is
recorded in the diagnostic preparation report.

## Reproduce and inspect

Small evidence: mesa-aco-prefetch-experiment-2026-11-10/ contains six original
summary/order pairs, correctness hashes, control summaries, trace comparison,
source patch and instruction/resource comparison. Large readbacks and console
logs remain under build/aco-runtime-trace/runs-approved-2026-11-10/.

Commands from the worktree root (Python steps run in WSL):

    python3 asi/tests/prepare_aco_prefetch_experiment.py
    python3 asi/build/build_aco_records.py build/aco-runtime-trace/experiment-projection-prefetch/data/shaders/dec_dfn_96_128_fl_lt.spv --out build/aco-runtime-trace/experiment-projection-prefetch/records --precision global --matrix-loads vector8
    python3 asi/tests/merge_aco_experiment_records.py build/aco-runtime-trace/experiment-projection-prefetch/records
    powershell -File asi/tests/run_runtime_trace_batch.ps1 -Stage Control -TraceName fast-prefetch-control -DataDir build/aco-runtime-trace/experiment-projection-prefetch/data -Records build/aco-runtime-trace/experiment-projection-prefetch/records/gfx1201/openrr-d3d12-v1 -Output build/aco-runtime-trace/runs-approved-2026-11-10
    powershell -File asi/tests/run_runtime_trace_batch.ps1 -Stage Trace -TraceFrame 6 -TraceName fast-prefetch-trace-6 -ReferenceName fast-prefetch-control -DataDir build/aco-runtime-trace/experiment-projection-prefetch/data -Records build/aco-runtime-trace/experiment-projection-prefetch/records/gfx1201/openrr-d3d12-v1 -Output build/aco-runtime-trace/runs-approved-2026-11-10
    powershell -File asi/tests/run_aco_prefetch_timing.ps1
    python3 asi/tests/inspect_aco_prefetch_experiment.py
    python3 asi/tests/summarize_aco_prefetch_experiment.py

Run the trace comparator with --out .../prefetch-trace6-diff.json before the
summary command. Ten CPU comparator tests, PowerShell parsing and
git diff --check passed. The timing script is specific to this isolated
candidate, not a new general benchmark framework.
