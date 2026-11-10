# enc96 MLP slice 16 experiment — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Decision: do not retain in shipping shaders. The isolated change reduces the
number of simultaneously live MLP hidden tiles, preserves all saved measured
outputs, but does not reduce allocated VGPR/scratch or demonstrate runtime
benefit. Global/vector8 and baseline shader source remain unchanged.

## Hypothesis and generated-code gate

Original enc96 W2 MLP uses 32-neuron slices: four hidden accumulators (two
channel tiles for each of two token tiles) coexist with 12 residual/output
tiles. After-spill IR uses four-dword FP16 WMMA accumulator tuples, while
FP8 Q/V elsewhere use eight-dword FP32 tuples. Shrinking only the hidden
slice to 16 retains two hidden accumulators and one weight fragment. Each
FC1 output still consumes channel tiles 0..5 in order; each FC2 accumulator
still consumes hidden tiles in increasing order, with identical per-MMA
rounding and GELU precision. Independent hidden outputs are produced earlier,
not reduced in a different order. No address variant is combined with this.

Original SPIR-V rebuilt byte-identically before the candidate was compiled.
Only enc_stage_96_128_128_fl_lt.spv differs in its isolated data folder.
Original body and candidate captures match the corresponding offline records.

| Resource | Original | MLP16 |
|---|---|---|
| SPIR-V FNV | 581ef3189b0a0052 | 77aac6515dddf7fe |
| VGPR / SGPR | 256 / 108 | 256 / 108 |
| Scratch B/thread | 8 | 8 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 36888 |

After-spill IR confirms the MLP body has two hidden accumulator chains instead
of four. Static FP16 WMMA count falls 212→188, global pointer-shift count
v_lshlrev_b64 159→146, but MLP loop iterations double. Therefore these static
reductions do not mean reduced dynamic matrix work. Scratch operations remain
two stores/two loads; the overall VGPR limit is unchanged. Full static counts
are in instruction-comparison.json. This is an observed body/liveness change,
not evidence of more resident waves.

## Correctness

Same actual Windows D3D12/PAL UploadPattern path, 1280x720→1920x1080,
flow/direct/sampler=1, global/vector8, phase alignment; explicit LLPC and
strict ACO. Separate correctness run: 8 warmup + 8 steady + 12 DRS, all
20 measured quality gates pass with no non-finite outputs. Both compilers'
20 outputs are byte-identical to their saved fast-aligned outputs.

Trace-6's 20 measured outputs match its own untraced control per compiler.
Each compiler's 69 binary intermediate snapshots match original trace-6.
The first live nonzero divergence remains attribute9 at dispatch3, pixel
(203,378); no new captured divergence is introduced. Earlier warmup frames
were not independently retraced. Structural trace comparison retains the
existing explicit pointer-address manual-review limitation. Legacy files are
unchanged; its saved exact control was not rerun for this Fast-only candidate.

## Runtime timing

Six paired strict-ACO runs, alternating order (three each), 24 warmup +
48 steady + 24 DRS; no trace, per-dispatch stamps or per-frame readbacks.
Flow/direct/output/sampler path logs checked every run.

| Median of six run medians, ms | Original | MLP16 | Candidate time change |
|---|---:|---:|---:|
| Steady | 3.71410 | 3.71684 | +0.074% |
| DRS | 3.55908 | 3.55110 | -0.224% |

Steady per-run medians: original 3.70196–3.73396, candidate
3.69932–3.73872. Paired changes -0.678% to +0.562%, three improve and
three regress. DRS ranges original 3.51592–3.57296, candidate
3.51496–3.63524; paired -1.320% to +1.979%. These data do not establish
an end-to-end gain. No further timing or profile is justified for this factor.
The unchanged resource allocation suggests another phase constrains the kernel.

## Reproduction and evidence

prepare_enc96_mlp16.py generates isolated source/SPIR-V; build_aco_records.py
captures global/vector8 and merge_aco_experiment_records.py supplies baseline
records. inspect_enc96_aco.py with label mlp16 verifies captured code against
the candidate code file; analyze_enc96_mlp16.py saves resource/count evidence.
run_runtime_trace_batch.ps1 Control/Trace use the isolated data/records, names
fast-enc96-mlp16-control/fast-enc96-mlp16-trace-6 and its own control as
ReferenceName. run_enc96_mlp16_timing.ps1 performs the six timing pairs.
After compare_runtime_trace.py writes enc96-mlp16-trace6-diff.json in the run
root, summarize_enc96_mlp16.py archives results and correctness hashes.

Small review data are in the adjacent mesa-aco-enc96-mlp16-experiment-2026-11-10/.
Full IR is build/aco-runtime-trace/experiment-enc96/ir/mlp16; isolated shader
and records are build/aco-runtime-trace/experiment-enc96-mlp16. GPU runs
are in runs-approved-2026-11-10. WSL is compiler/CPU-only; all timing uses
Windows D3D12/PAL. No baseline table or rejected prefetch test was repeated.
All this stage's changes remain uncommitted.
