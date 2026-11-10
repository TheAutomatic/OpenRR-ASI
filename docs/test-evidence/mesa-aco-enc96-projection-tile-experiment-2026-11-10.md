# enc96 existing tiled projection weights — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Decision: do not promote. NT6 output projection's OLD macro uniquely loads
the canonical row-major weights while the generic OUTP path uses the
already packed 16x16 copy. Host engine.cpp with_tiles appends that exact
copy and uploads it for every block output matrix. This experiment changes
only which existing copy OLD addresses; weight values, prefetch depth, MMA
order, rounding and global/vector8 remain unchanged. It is not a repeat of
the rejected reduced-prefetch experiment.

## Compile gates

First form used WT_OUT/WS_OUT directly. It compiled to the desired tile
addresses but introduced generic dynamic division/remainder operations: static
s_bfe_u32 1→13, s_and_b32 17→29, s_add_u32 200→224, s_lshl_b32 35→47;
code grew from 39544 to 39944 executable bytes. VGPR256 and scratch8
unchanged. This generic form was rejected without GPU execution; its
SPIR-V FNV is 4d6d3ef581f2e232 and code-gate data are in
mesa-aco-enc96-projection-tiled-codegate-2026-11-10/.

The next isolated form explicitly computes the same tiled base:

    C*X + channel_tile*16*X + head*512 + half*256

with stride16. For the current head0..3, channel tile0..5, half0..1 this
equals WT_OUT of the original matrix start. with_tiles maps every element
at r*K+k to n+(r/16)*16*K+(k/16)*256+(r%16)*16+k%16, so each 16x16
matrix operand contains identical half bits. No packing/runtime change is
needed. Only the selected enc96 Fast flow/lite SPIR-V is replaced.

| Resource/identity | Original | Direct tiled index |
|---|---|---|
| SPIR-V FNV | 581ef3189b0a0052 | 5e2bfcb0eac64100 |
| ACO body FNV | 265363938b7396d2 | 94196c774ba248a1 |
| VGPR / SGPR | 256 / 108 | 256 / 108 |
| Scratch B/thread | 8 | 8 |
| LDS B/workgroup | 12288 | 12288 |
| Executable bytes | 39544 | 39560 |

Original SPIR-V rebuild and both offline code recaptures match baseline and
candidate records byte for byte. Generic tiled-index extra scalar operations
are absent in this form. Static loads/MMA counts remain unchanged, with
s_wait_loadcnt 141→131 and s_waitcnt_depctr 830→820; these counts do not
measure dynamic stalls. The meaningful change is weight memory locality,
not fewer arithmetic operations or a lower register allocation.

## Correctness and timing

Same phase-aligned actual runtime, Windows D3D12/PAL, 1280x720→1920x1080,
flow/direct/sampler=1, global/vector8. Separate 8 warmup + 8 steady + 12
DRS control passes all20 Fast frame gates, no non-finite values. Both
compilers'20 outputs match original fast-aligned byte for byte. Trace6
matches its own control on measured outputs, and all69 original intermediate
binary snapshots per compiler. The saved dispatch3 attribute9 first divergence
is unchanged; prior warmup frames were not independently retraced. Manual
pointer-address-review limitation remains explicit. Legacy files unchanged.

Six strict-ACO original/candidate pairs rotate execution order, 24 warmup +
48 steady + 24 DRS, trace/per-dispatch stamps/per-frame readbacks off.
Actual paths checked each run.

| Median of six run medians, ms | Original | Candidate | Time change |
|---|---:|---:|---:|
| Steady | 3.73370 | 3.74070 | +0.187% |
| DRS | 3.56962 | 3.56086 | -0.245% |

Steady ranges: original 3.72672–3.74692, candidate 3.70808–3.74972;
paired -0.550% to +0.617%, four of six regress. DRS ranges: original
3.53052–3.63372, candidate 3.53952–3.57276; paired -2.508% to +0.960%.
No stable end-to-end improvement. Keep neither form in production source.

## Evidence and reproduction

prepare_enc96_projection_tiled.py creates the rejected generic form;
prepare_enc96_projection_tile_offset.py creates the direct-index form.
Both build with the original flags; build_aco_records.py global/vector8 and
the record merge utility prepare isolated test sets. Inspector label
projection-tile-offset writes full IR; analyze_enc96_projection_tile_offset.py
records static counts. Control/Trace names fast-enc96-projection-tile-control
and fast-enc96-projection-tile-trace-6 use their own control reference.
run_enc96_projection_tile_timing.ps1 runs timing. Trace comparison writes
enc96-projection-tile-trace6-diff.json before summarize_enc96_projection_tile.py.

Small evidence is in mesa-aco-enc96-projection-tile-experiment-2026-11-10/.
Full build artifacts are in experiment-enc96-projection-tile-offset and
experiment-enc96/ir/projection-tile-offset beneath build/aco-runtime-trace/.
WSL is offline compiler/CPU only. No baseline package changes or new commit.
