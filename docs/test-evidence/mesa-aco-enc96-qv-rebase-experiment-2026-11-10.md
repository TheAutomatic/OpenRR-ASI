# enc96 Q/V pointer rebasing: rejected at code gate — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Actual baseline Q/V after-spill IR repeats s_add_u32 plus s_ashr_i32 by 31,
pair construction and s_add_u64 for each dynamic coop-matrix tile offset.
The independent candidate adds wq to the E4View BDA once per head, then
uses literal per-tile offsets. This preserves byte addresses for the fixed
nonnegative head/tile range and does not change matrix arithmetic.

Original shader rebuilt byte-identically; candidate only changes
enc_stage_96_128_128_fl_lt.spv. SPIR-V FNV 5298e1fca6efca3d. Offline
capture matches saved candidate body. Global/vector8 unchanged.

| Resource | Original | Rebased |
|---|---:|---:|
| VGPR | 256 | 256 |
| SGPR | 108 | 108 |
| Scratch B/thread | 8 | 16 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 39112 |

Reject before GPU: code shrinks but scratch doubles and the expected resource
improvement is absent. No GPU correctness or performance result is claimed
for this candidate. No test table is run merely because a shader compiles.

prepare_enc96_qv_rebase.py and its source-change.patch reproduce the isolated
change. Full capture/IR is under build/aco-runtime-trace/experiment-enc96/ir/qv-rebase
and experiment-enc96-qv-rebase; adjacent small evidence preserves candidate
manifest, resources and patch. Baseline shader/source is not modified.
