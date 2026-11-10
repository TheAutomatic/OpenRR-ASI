# enc96 token tiling and DFN precision audit — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Target: enc_stage_96_128_128_fl_lt. Baseline checkpoint ba817e3,
Mesa 96cb43121031992b85767f9c1be8f3f48e22b1d2, capture v4,
global/vector8, strict ACO on Windows D3D12/PAL. No LLPC fallback.

## Hypothesis and machine-code evidence

The W2 body keeps two 16-token tiles in each wave, including residuals,
Q/V and output chains. The actual baseline spills two integer address
components across long matrix live ranges (see the address experiment).
Removing those spills alone did not establish runtime benefit. The next
factor selects the existing transposed body, W2=0: one 16-token tile per
wave, four waves per window instead of two. Host dispatch groups still
cover one workgroup per window. This is a body/tiling choice; its LDS,
loading, scheduling and workgroup changes are coupled consequences, not
independently controlled improvements.

| Resource | Original W2 | W2=0 | W2=0, original DFN policy |
|---|---:|---:|---:|
| SPIR-V FNV | 581ef3189b0a0052 | 91263369a61061b4 | f3817f3fb0a26545 |
| ACO code FNV | 265363938b7396d2 | c1629007fde5ef4f | c08996618a61cd08 |
| VGPR / SGPR | 256 / 108 | 240 / 108 | 240 / 108 |
| Scratch B/thread | 8 | 0 | 0 |
| LDS B/workgroup | 12288 | 19456 | 19456 |
| Threads/workgroup | 64 | 128 | 128 |
| Wave size | 32 | 32 | 32 |
| Executable bytes | 39544 | 38068 | 38028 |

Generated final HW IR confirms both scratch stores/reloads disappear.
Static v_add_co_u32/v_addc_co_u32 pairs fall 455→143, v_lshlrev_b64
159→51, v_dual_mov_b32 151→99, and s_waitcnt_depctr 830→334.
Other costs increase: global_load_dwordx4 167→422, ds_read_b64
92→188, barrier signals/waits 7→9 each. Counts describe code sites,
not executed instructions: different wave counts, loop structure and
per-wave work prohibit using these totals as dynamic traffic or speed.
LDS rises and threads double; no measured residency claim is made.

This applies Mochi's shape-specific tile/resource measurement practice.
The implementation uses RR's existing transposed body and weight copies;
no NR code or weight packing is copied. NR's descriptor-backed arena
and network dimensions are not substituted for RR's BDA contract.

## Initial correctness and timing, before the precision audit

The original W2=0 candidate changes only the selected SPIR-V. For the
accepted phase-aligned 8 warmup + 8 steady + 12 DRS sequence, every one
of the 20 outputs per compiler matches its original baseline byte for
byte. The frame6 trace matches its own untraced outputs, and all 69
intermediate readbacks per compiler match the original trace6. Thus the
saved LLPC/ACO dispatch3 attribute9 case remains unchanged on this short
sequence. This does not establish equality for arbitrarily long history.

Timing uses strict ACO versus strict ACO, identical runtime and inputs,
1280x720→1920x1080, flow/direct guides/output/sampler=1. Orders alternate
for six pairs. No intermediate trace or per-frame readback; quality and
timing are separate. All path diagnostics are checked.

| Median of six run medians, ms | Original | Initial W2=0 | Time change |
|---|---:|---:|---:|
| Steady, 24 warmup/48 frames | 3.73406 | 3.69416 | -1.069% |
| DRS, 24 frames | 3.56640 | 3.55310 | -0.373% |
| Steady confirmation, 48 warmup/240 frames | 3.76332 | 3.73462 | -0.763% |
| DRS confirmation, 64 frames | 3.53476 | 3.52012 | -0.414% |

Initial steady ranges: 3.71956–3.74652 vs 3.68816–3.71356; all six
paired changes improve (-0.535% to -1.510%). Confirmation steady ranges:
3.72340–3.76836 vs 3.71732–3.74076; five improve, one +0.038%.
Confirmation DRS ranges: 3.49772–3.55028 vs 3.47188–3.56864;
five improve, one +1.128%. The small DRS effect has visible variability.

Two reversed-order diagnostic profiles (24 warmup/640 steady/24 DRS)
report target means 0.2293/0.2294 ms original versus 0.1738/0.1744 ms
candidate, about 24% less. The longest logged interval has 596 samples
and may include late warmup. Instrumentation changes scheduling; use
the unstamped table for runtime benefit. These measurements belong to
the initial candidate and cannot be assigned to a corrected binary.

## Extended correctness exposed a policy mismatch

Review of the confirmation summaries showed nonzero ACO/ACO output
differences despite the short sequence's equality. A separate correctness
run, 8 warmup + 280 steady + 12 DRS, reads every measured frame. Frame
states match. Steady outputs 0..130 are byte-identical. The first different
output is steady index131, Engine139 (phase3), with 19 changed halfwords,
max5 ULP. First output halfword1367224 changes 0x3259→0x3256.
All 292 quality gates pass with no nonfinite values. After temporal
accumulation the last steady output has max61 ULP and PSNR59.6335 dB;
all 12 DRS outputs become exact after the existing reset sequence.
This ACO/ACO comparison is not the original LLPC/ACO error baseline.

A complete frame139 trace reproduces each compiler's untraced output
hash exactly. All captured entry guides/history and dispatch0..5
readbacks match. First differences are dispatch6 enc96 DFN output:
two changed FP16 values (first halfword4121: 0x3128→0x3126), and its
pooled DFN. Tensor and pooled tensor outputs match. Later DFN stages
propagate the change. Structural comparison checks groups/indirect,
non-address push, matrices and guide metadata; pointer equivalence still
requires the comparator's documented resource/offset review.

Source review identifies an actual policy difference between the existing
bodies: W2's encoder DFN uses SIG_RCP, selecting ordinary division under
LT_HM; the transposed encoder DFN always uses rcp_rn. Thus simply
selecting W2=0 changes a precision rule as well as tiling. Short-sequence
equality did not reveal it. The initial candidate is superseded, pending
the isolated corrected test, even though existing Fast gates passed.

The corrected candidate keeps W2's SIG_RCP policy in this enc96 variant.
ENC_DFN_SIG_RCP scopes the override to the measured flow/lite variant.
No other shader variant receives it. Other arithmetic boundaries, head
count, hidden width, matrix-load policy and global exact are unchanged.
The retained-source verification rebuilds _f, _fl, _f_lt byte-identical
to the checkpoint and _fl_lt byte-identical to the isolated correction.
Corrected correctness/timing results are recorded separately below.

## Corrected candidate: retained

With the original DFN policy restored, the separate 8 warmup + 280 steady
+ 12 DRS correctness run is byte-identical to the original strict ACO for
all 292 measured frames, with no nonfinite values. Frame139's complete
69-readback trace is also byte-identical to original ACO, including DFN
and pooled DFN. Traced output hashes match the corresponding untraced
steady index131 for both sides. This isolates the prior observed new
DFN differences to the reciprocal-policy mismatch on the tested case.

The corrected 20-frame explicit LLPC/ACO control reproduces the accepted
outputs byte for byte for each compiler. Its frame6 trace reproduces
the untraced outputs and all 69 original readbacks per compiler. The
accepted LLPC/ACO first live nonzero case is unchanged. Legacy source,
SPIR-V and records remain unchanged; its accepted exact control is reused.

Six new rotated strict-ACO pairs use 48 warmup + 240 steady + 64 DRS.
All six phase-final outputs now match byte for byte as well. No quality
readbacks or stamps are enabled inside the measured frame sequence.

| Median of six run medians, ms | Original | Corrected candidate | Time change |
|---|---:|---:|---:|
| Steady | 3.76140 | 3.73490 | -0.705% |
| DRS | 3.54492 | 3.52402 | -0.590% |

Steady original/candidate ranges: 3.74788–3.76420 / 3.72372–3.74056 ms.
All six steady pairs improve, -0.365% to -1.075%. DRS ranges:
3.51544–3.55540 / 3.47212–3.54672 ms; four improve and two regress,
paired -2.110% to +0.890%. Retain the small, repeatable steady runtime
gain; DRS has a favorable median with greater variability. The earlier
24% per-kernel profile belongs to the initial candidate, not a separate
profile of this corrected binary.

The production source retains only this corrected enc96 flow/lite choice.
No rejected experiment is stacked onto it. The final source verification
proves its SPIR-V equals f3817f3fb0a26545; other Fast variants equal the
checkpoint. The staged ASI/shader directory and baseline records still
contain the original accepted baseline. Tested updated artifacts are in
the isolated experiment-enc96-wave-rcp directory, ready for review.

## Evidence and reproduction

The adjacent directory contains short-run results/hashes, all rotated
summaries, confirmation frame-state CSVs, profile logs, source verification
and compiler-evidence.zip. The archive includes IR before scheduling,
after spill and final HW lowering for all enc96 experiments, plus the
tested initial SPIR-V/record; compiler-evidence.json checks every member.
Corrected payloads are added after their tests. No staged/release package
is overwritten, and changes after ba817e3 remain uncommitted.

From the worktree root, WSL CPU/offline capture:

    python3 asi/tests/prepare_enc96_wave_tiles.py
    python3 asi/tests/prepare_enc96_wave_rcp.py
    python3 asi/build/build_aco_records.py build/aco-runtime-trace/experiment-enc96-wave-rcp/data/shaders/enc_stage_96_128_128_fl_lt.spv --out build/aco-runtime-trace/experiment-enc96-wave-rcp/records --precision global --matrix-loads vector8
    python3 asi/tests/merge_aco_experiment_records.py build/aco-runtime-trace/experiment-enc96-wave-rcp/records
    python3 asi/tests/verify_enc96_retained_source.py

Windows GPU correctness: run_enc96_wave_long_correctness.ps1 with
-Experiment experiment-enc96-wave-rcp and a new -Name. Timing:
run_enc96_wave_tiles_timing.ps1 -Experiment experiment-enc96-wave-rcp
with a new -Output, and -Warmup 48 -Frames 240 -DrsFrames 64.
Do not reuse output directories or interpret correctness-run timestamps.
Preparers explicitly restore the checkpoint shader, including Windows
worktree gitdir translation under WSL, so retaining a source change
cannot silently change an experiment's control.

Final compiler-evidence.zip: 2,155,047 bytes, SHA256
50b1d0f3b7179d3e5842acd78aa424496ebb9adc8adb703d27bbfa7e630f4b3f.
The corrected/ subdirectory contains the six raw summaries, logs, frame
states, manifest, patch and per-output/per-readback hashes. The separate
enc96-wave-rcp-long-correctness/ contains the full 292-frame summary and
hash list. Original failed-policy long-run data remain under
long-correctness/, and its frame139 evidence is under initial-trace139/.
All 18 CPU tests, Python compile checks and PowerShell parse checks pass.
