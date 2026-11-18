# enc96 independent Q/V scheduling — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Decision: reject for production. This independent factor serializes the Q
and V projections while preserving each output's six K steps and one FP32→FP16
conversion boundary. It does not combine MLP16, merge-address or prefetch
changes. Global/vector8 and the accepted baseline remain unchanged.

## Machine-code evidence and hypothesis

Baseline after-spill IR contains eight simultaneous FP32 WMMA chains with
v8 (eight-dword) results, four Q and four V. For example %1010..%1017 start
these eight chains, with %1036..%1043 consuming them at the next K tile.
The candidate completes and converts four Q chains before starting four V
chains. Generated IR actually follows that order (first chain results
%1006..%1009, next K %1024..%1027). Thus the intended live FP32 matrix-state
reduction occurred, but allocated resources did not improve.

| Identity/resource | Baseline | Q/V serial |
|---|---|---|
| SPIR-V FNV | 581ef3189b0a0052 | 9d9d6ffe24d1262f |
| ACO code FNV | 265363938b7396d2 | 1ccfddd677387599 |
| VGPR / SGPR | 256 / 108 | 256 / 108 |
| Scratch B/thread | 8 | 8 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 39292 |

Original shader rebuild is byte-identical; only the selected Fast flow/lite
SPIR-V differs in the isolated directory. Compiler IR captures match code
bodies. WMMA and memory operation counts are unchanged; changed scheduling
and register moves are saved in instruction-comparison.json. No residency
increase is inferred from smaller simultaneous FP32 state in this phase.

## Correctness and runtime

Separate 8 warmup + 8 steady + 12 DRS correctness control on actual Windows
D3D12/PAL, 1280x720→1920x1080, phase-aligned, direct/sampler/flow=1. All
20 Fast quality gates pass with no non-finite values. All 20 outputs per
compiler are byte-identical to the original fast-aligned outputs. Trace6
matches its own untraced control outputs and all 69 original intermediate
readbacks per compiler. The saved dispatch3 attribute9 first divergence
does not move; earlier warmup was not independently retraced. Existing manual
pointer-address review limitation remains explicit. Legacy files are unchanged.

Six strict-ACO original/candidate timing pairs rotate order (three each),
24 warmup + 48 steady + 24 DRS, no trace/per-dispatch stamps/per-frame
readbacks, actual path logs checked.

| Median of six run medians, ms | Original | Q/V serial | Time change |
|---|---:|---:|---:|
| Steady | 3.72392 | 3.72684 | +0.078% |
| DRS | 3.54110 | 3.57492 | +0.955% |

Steady ranges: original 3.70548–3.73952, candidate 3.70660–3.73240;
paired -0.741% to +0.635%, three improve and three regress. DRS original
3.51972–3.60744, candidate 3.54468–3.62176, paired -0.437% to +1.450%,
five of six regress. There is no demonstrated runtime gain.

## Evidence/reproduce

prepare_enc96_qv_serial.py generates isolated shaders; build_aco_records.py
uses global/vector8, followed by merge_aco_experiment_records.py. Inspector
label qv-serial writes IR into experiment-enc96/ir/qv-serial.
analyze_enc96_qv_serial.py saves counts. Control/Trace names are
fast-enc96-qv-serial-control and fast-enc96-qv-serial-trace-6, with
ReferenceName selecting that control. run_enc96_qv_serial_timing.ps1 runs
timing. compare_runtime_trace.py output is enc96-qv-serial-trace6-diff.json;
summarize_enc96_qv_serial.py archives small data/hashes in the adjacent
mesa-aco-enc96-qv-serial-experiment-2026-11-10/ directory.

All GPU execution is Windows D3D12/PAL; WSL only builds/captures/analyzes.
No native Linux GPU timing, LLPC table repeat or precision-policy change.
Uncommitted experiment source/evidence only; baseline package untouched.
