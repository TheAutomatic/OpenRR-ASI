# enc96 merge-address experiment — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


The isolated enc_stage_96_128_128_fl_lt candidate removes the baseline's
8 B/thread scratch and preserves the saved correctness results. A runtime
per-dispatch profile shows a small local improvement, but six uninstrumented
runtime pairs do not establish a stable end-to-end benefit. Do not promote
this variant into shipping shaders. The accepted global/vector8 baseline
and checkpoint commit ba817e3 remain the reference. This experiment is
uncommitted and does not repeat the accepted LLPC-versus-ACO baseline.

## Actual spill and long-lived values

Original SPIR-V 581ef3189b0a0052 and ACO body 265363938b7396d2 were recaptured
with compiler internal representations; the body is byte-identical to the
saved baseline. These are offline ACO representations of the actual imported
code, not a claim to observe Windows driver-final registers.

The after-spill IR identifies precisely two spilled VGPR words:

- %123 = v_and_b32 7, %122, with %122 = lane_low4 + 32*subgroup + 16.
  This is the low token-column component for token tile B. It is defined at
  IR line 141, stored before the head loop at line 921, and reloaded at line
  4107 for the merge LDS staging address.
- %871 = v_lshrrev_b32 4, lane_id, defined at line 847. It is the lane half
  index, stored before the head loop at line 922 and reloaded at line 5554
  for the merge output LDS address after the merge matrix computation.

Each has one static scratch_store_dword and one scratch_load_dword. The
excerpts retain original IR line numbers in original-spill-excerpt.txt. These
are integer address components, not spilled matrix accumulators. Their live
ranges bridge attention, output projection and MLP work before the merge
consumes them. Line distances describe code location, not dynamic latency.
The kernel still has 256 allocated VGPRs; no cycle-accurate or compiler peak
live-register count is inferred from this evidence.

The original merge MST expression reconstructs token t, splits its row/column
bits, builds a half-element offset, then divides by eight for lds4. Keeping
its early token component alive contributes to the observed spill. The
candidate directly computes the equivalent lds4 index from lane row/column
components. It reduces integer address work without changing a load, store,
matrix operation, barrier, weight layout or numerical precision boundary.

This follows the reference project's concrete practice of inspecting invariant
fragment/lane offsets and live ranges instead of assuming fewer source lines
are faster. No Mochi code or weight layout is copied. It is separate from
the rejected decoder projection-prefetch experiment.

## Single factor and generated-code gate

Only the MST address expression in an isolated copy of onr_block_w2.glsl is
changed. For each lane, with t0=32*subgroup+16*token_tile:

    row = (t0 >> 4)*4 + ((lane >> 1) & 3)
    sub = ((lane >> 2) & 2) | (lane & 1)
    lds4_index = row*(ENC_C/2) + sub*(ENC_C/8) + channel_tile*2 + (lane >> 4)

All 768 subgroup/lane/token-tile/channel-tile combinations were checked on
CPU against the old integer expression for C=96. The original shader rebuild
matches the shipping SPIR-V byte for byte. Only enc_stage_96_128_128_fl_lt.spv
is changed in the experiment data directory. No staged shader is overwritten.

| Identity / resource | Original | Candidate |
|---|---|---|
| SPIR-V FNV | 581ef3189b0a0052 | bd2f725d3d958594 |
| ACO code FNV | 265363938b7396d2 | 9ce4446a3747b7b7 |
| VGPR | 256 | 256 |
| SGPR | 108 | 108 |
| Scratch B/thread | 8 | 0 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 39148 |
| Wave / workgroup | 32 / 64 | 32 / 64 |

Generated-code checks passed before GPU testing. All scratch operations vanish.
Static v_add3_u32 count drops 42→31, v_lshrrev_b32 113→98, v_lshlrev_b32
54→43 and v_lshl_add_u32 89→85; v_mad_u32_u24 increases 4→6. Integer
reassociation also changes waits/register moves. WMMA, global-load and LDS
operation counts are unchanged. Full count differences are in
instruction-comparison.json. These are static HW IR counts, not executed
instruction counts. VGPR allocation remains at the same residency limit;
scratch elimination alone does not demonstrate more resident waves.

## Correctness gate

Actual Windows D3D12/PAL runtime, direct guides/output, sampler and flow on;
global/vector8 unchanged; 1280x720→1920x1080; same phase alignment, input
pattern, jitter, projection, reset and DRS sequence as the accepted baseline.

The isolated control runs explicit LLPC versus strict ACO, 8 warmup + 8
steady + 12 DRS frames. All 20 per-frame quality gates pass, no non-finite
outputs. Each compiler's 20 outputs match the corresponding saved fast-aligned
output byte for byte. This checks the candidate against the original ACO too,
not merely against a newly compiled LLPC shader.

The candidate's trace-6 run matches its own untraced control for all 20
measured outputs per compiler. All 69 binary intermediate readbacks per
compiler match the original trace-6 readbacks byte for byte. The saved first
live nonzero difference remains dispatch 3 d3d_enc0_attr_f_lt, attribute 9,
pixel (203,378), byte 23234111, LLPC 0x6e versus ACO 0x6f. The captured entry
guides/history match the earlier case. Earlier warmup frames were not
independently retraced for this candidate.

The structural comparator checks non-address push fields, groups/indirect,
projection and guide metadata. Its existing explicit manual address-review
limitation is retained; no new automatic pointer-normalization claim is made.
Legacy SPIR-V and records are unchanged, so the saved 20-frame Legacy exact
control remains the baseline; no new Legacy GPU run is claimed.

## End-to-end timing and targeted profile

Six strict-ACO original/candidate pairs, 24 warmup + 48 steady + 24 DRS,
three runs of each order alternating. Same ASI/benchmark, actual Windows
D3D12/PAL. No trace, no per-frame readback, no per-dispatch stamps. Actual
flow/direct/sampler paths are checked for every run. The phase-final checks
pass but are not substituted for the earlier all-frame quality run.

| End-to-end statistic, ms | Original | Candidate | Candidate time change |
|---|---:|---:|---:|
| Median of six steady medians | 3.73612 | 3.72690 | -0.247% |
| Steady per-run median range | 3.71620–3.75788 | 3.72004–3.75240 | paired -0.790% to +0.464% |
| Median of six DRS medians | 3.56992 | 3.57356 | +0.102% |
| DRS per-run median range | 3.52908–3.65428 | 3.52868–3.64012 | paired -1.157% to +2.870% |

Steady pairs improve in three rounds and regress in three. These data do
not establish stable runtime benefit. Absolute timings are from this window,
not compared against a different window to infer acceleration. No accepted
LLPC/ACO baseline rerun was performed.

Two further diagnostic pairs enable ONR_RR_STAMPS, reverse order, and use
24 warmup + 640 steady + 24 DRS. The runtime's periodic log supplies the
longest fixed-size interval (596 samples per side, before DRS; the collection
can include late warmup). The dispatch index is taken from stamped-dispatch
names; its end-minus-start interval is extracted, not an adjacent gap.

| Profile order | Original target kernel mean ms | Candidate target kernel mean ms |
|---|---:|---:|
| Original first | 0.2282 | 0.2252 |
| Candidate first | 0.2303 | 0.2252 |

The local gain is about 1.3–2.2%, or 3–5 microseconds. Per-dispatch stamps
perturb scheduling, so these means explain the limited scale of the local
change and are not used as the final end-to-end measurement.

## Decision and next question

Keep the isolated variant and small evidence only; do not apply the shader
change to the production source. It is an informative scratch-free control,
not a validated runtime optimization. No precision change or fallback was
introduced. The result narrows the scope: removing these two integer spills
does not by itself address the main performance gap.

A next candidate should investigate the 256-VGPR live matrix state across
Q/V, residual and MLP work, or repeated 64-bit weight address construction,
with an observable allocation/dependency improvement. It must be a separate
factor and preserve the original accumulation/rounding sequence. No second
variant is implemented in this report.

## Reproduce and evidence

Use WSL for Python/compiler capture only. GPU .exe calls execute through
Windows D3D12/PAL. Scripts from the worktree root:

    python3 asi/tests/inspect_enc96_aco.py
    python3 asi/tests/prepare_enc96_experiment.py
    python3 asi/build/build_aco_records.py build/aco-runtime-trace/experiment-enc96/data/shaders/enc_stage_96_128_128_fl_lt.spv --out build/aco-runtime-trace/experiment-enc96/records --precision global --matrix-loads vector8
    python3 asi/tests/inspect_enc96_aco.py merge-address build/aco-runtime-trace/experiment-enc96/data/shaders/enc_stage_96_128_128_fl_lt.spv build/aco-runtime-trace/experiment-enc96/records/gfx1201/openrr-d3d12-v1/capture/bd2f725d3d958594-a.code
    python3 asi/tests/merge_aco_experiment_records.py build/aco-runtime-trace/experiment-enc96/records

Run run_runtime_trace_batch.ps1 Control/Trace with data and record roots above,
names fast-enc96-address-control/fast-enc96-address-trace-6, TraceFrame 6 and
ReferenceName fast-enc96-address-control. Then:

    powershell -File asi/tests/run_enc96_timing.ps1
    powershell -File asi/tests/run_enc96_timing.ps1 -Profile -Output build/aco-runtime-trace/runs-approved-2026-11-10/enc96-address-profile
    python3 asi/tests/analyze_enc96_ir.py
    python3 asi/tests/summarize_enc96_profile.py

Run compare_runtime_trace.py on the paired trace-6 folders, with output
.../runs-approved-2026-11-10/enc96-trace6-diff.json, before running
summarize_enc96_experiment.py. The existing ten CPU comparator tests and
PowerShell parse pass. No source change requires rebuilding the runtime.

Small evidence (approximately 280 KB) is in the adjacent
mesa-aco-enc96-address-experiment-2026-11-10/ directory. Full IR and isolated
shader/record artifacts remain in build/aco-runtime-trace/experiment-enc96/.
Large new correctness readbacks are in the matching runs-approved directories
and can be discarded after review; the original saved cases remain untouched.
