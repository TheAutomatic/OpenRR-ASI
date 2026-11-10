# enc96 residual scheduling — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Decision: reject for production. Delaying the FP16 residual's creation removes
scratch and many register moves, but does not reduce allocated VGPRs or show
consistent end-to-end improvement. Keep as an isolated scheduling experiment.

## Hypothesis and actual generated code

XIN2Y originally creates both FP16 residual x tiles and packed FP8 Q/V y
tiles before attention. The FP16 x tiles are unused until RESID1 after the
head loop. This causes 12 FP16 residual tiles to coexist with the packed
inputs, eight FP32 Q/V chains and output accumulators. The source candidate
creates packed inputs first, and places FP16 residual decoding at its first
use. It does not change the decoder, matrix order, weight packing, barriers
or precision. Original input pc.x is read-only through this region.

Compiler CSE keeps the same global-load count (so this is not evidence of
extra memory reads). The actual improvement is a changed decoding/register
schedule. Final HW IR has v_dual_mov_b32 151→98, v_mov_b32 21→13 and
v_swap_b32 3→0. The two scratch stores and two reloads disappear. All WMMA,
global and LDS operation counts remain unchanged. The label residual-reload
describes the source construction, not an observed second global load.

| Resource/identity | Original | Candidate |
|---|---|---|
| SPIR-V FNV | 581ef3189b0a0052 | 064931df5d4379a7 |
| VGPR / SGPR | 256 / 108 | 256 / 108 |
| Scratch B/thread | 8 | 0 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 39048 |

Original SPIR-V rebuild matches byte for byte; only the selected Fast
flow/lite shader differs in the isolated directory. Offline recapture matches
both saved code bodies. No different precision/loading policy is introduced.

## Correctness before performance

Actual Windows D3D12/PAL runtime, global/vector8, phase alignment, direct
guides/output/sampler/flow=1, 1280x720→1920x1080. Separate 8 warmup + 8
steady + 12 DRS correctness run saves all 20 measured outputs. All gates
pass, no non-finite values; all 20 outputs per compiler match fast-aligned
byte for byte. Both explicit LLPC and strict ACO use the candidate shader.

Trace6 matches its own untraced control on all 20 measured outputs. All 69
intermediate binary readbacks per compiler match the original trace6. The
saved dispatch3 attribute9 case at pixel (203,378) is unchanged. Earlier
warmup frames were not independently retraced. The structural comparator
retains its explicit manual address-review limitation. Legacy shaders and
records are unchanged; the saved 20-frame exact control is reused.

## Six rotated timing pairs

Same strict ACO runtime, 24 warmup + 48 steady + 24 DRS, three runs of
each order alternating. Trace/per-dispatch stamps/per-frame readbacks off;
actual path logs checked. Windows D3D12/PAL timing, not WSL GPU timing.

| Median of six run medians, ms | Original | Candidate | Time change |
|---|---:|---:|---:|
| Steady | 3.72464 | 3.73720 | +0.337% |
| DRS | 3.57730 | 3.55646 | -0.583% |

Steady ranges: original 3.71160–3.75084, candidate 3.72704–3.75652;
paired -0.399% to +0.739%, five of six regress. DRS ranges: original
3.56444–3.61480, candidate 3.54176–3.60504, paired -1.330% to +0.910%.
Do not promote a candidate with these inconsistent effects. Eliminating
spill/moves alone is not sufficient for the intended runtime benefit.

## Review data and scripts

prepare_enc96_residual_reload.py creates source-change.patch and isolated
SPIR-V, build_aco_records.py captures global/vector8, followed by the merge
utility. inspect_enc96_aco.py label residual-reload verifies actual code;
analyze_enc96_residual_reload.py saves static counts. Control/Trace names
fast-enc96-residual-control/fast-enc96-residual-trace-6 use their own control
as ReferenceName. run_enc96_residual_timing.ps1 runs timing, and the trace
comparison writes enc96-residual-trace6-diff.json before
summarize_enc96_residual.py archives data.

Small data are in the adjacent mesa-aco-enc96-residual-experiment-2026-11-10/.
Full IR is experiment-enc96/ir/residual-reload. Isolated SPIR-V/records are
experiment-enc96-residual-reload. No production shader, staged package or
accepted baseline is changed. All work in this stage remains uncommitted.
