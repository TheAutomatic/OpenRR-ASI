# Other W2 kernels: offline single-tile screen — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Four actual runtime Fast variants were compiled with the pinned Mesa,
capture v4, global/vector8, wave32. This stage only builds SPIR-V and
captures ACO code with the noop shim; it makes no GPU performance claim.
Original SPIR-V rebuilds equal the staged files byte for byte. Production
shader sources are untouched by this screen.

## Factor and numerical expectations

Select the existing transposed body with W2=0: one 16-token tile per wave
and four waves per window, instead of two tiles/wave and two waves/window.
The coupled body changes include weight reuse, register scheduling, LDS
layout and barriers. This is not an independent occupancy-only experiment.
Workgroup grid coverage is unchanged; local threads64→128.

Both bodies preserve the per-accumulator matrix K order, FP8 decode and
FP16 accumulator boundaries, softmax sum tree, GELU and residual FMA
policy. Some independent operations are scheduled differently. No intentional
numerical change is expected, but Fast is judged with existing quality
limits and first-divergence analysis rather than a universal bit-exact gate.

The decoder DFN was checked specifically: both bodies call the same
sigmoid16 (SIG_RCP under LT_HM) and upsample_below. The latter preserves
the 0.25/0.75 parity weights, reflected coordinates, FP16 products and
explicit FMA sequence. DFN skip blend, alpha and saturation also match.
Unlike enc96's initial experiment there is no rcp_rn/SIG_RCP mismatch.

enc_stage64's transposed encoder DFN receives ENC_DFN_SIG_RCP=1, keeping
the current W2 SIG_RCP rule. Its input merge matrix order/bias, output
merge, pooling order and FP8 conversion stay unchanged. This is preservation
of the original precision policy, not an additional approximation.

dec_up64's W2 main traverses workgroups in reverse window order, while the
generic body defaults to forward order. The isolated candidate preserves
the original reverse traversal to avoid adding an unrelated dispatch-order
change to the tiling experiment. Its upsample projection still uses the
same weights, bias, K order, FP16 WMMA and skip FMA.

## Compiler screen

All entries SGPR108, scratch0, wave32. LDS is bytes/workgroup, code size is
ACO exec_bytes rather than ELF/container size. Full hashes and static
final-HW instruction counts are in the adjacent results.json.

| Actual runtime kernel | SPIR-V original → candidate | VGPR | LDS B | Executable B | Gate |
|---|---|---:|---:|---:|---|
| dec_dfn_96_128_fl_lt | 9b09c8e4e1c7b892 → a49ddbc0d311dedb | 256→256 | 9728→31744 | 27560→32692 | Reject before GPU |
| dec_dfn_64_64_f_lt | 52722875f5de9eb1 → 8df4481367098c34 | 240→240 | 9728→17920 | 29096→17508 | Shortlist, second |
| dec_up_64_64_96_f_lt | 40dc71ff1cd2b848 → bbfd504de9eb12ec | 256→240 | 9728→17920 | 31108→16996 | Shortlist, first |
| enc_stage_64_64_96_f_lt | 8cca057e36e2b4cc → 84206f86ce5fb965 | 256→256 | 9728→27648 | 36620→22656 | Defer, no GPU now |

dec96's original hot-kernel priority does not justify testing an unpromising
binary: no register reduction, larger code, over triple LDS and additional
barriers. A separate LDS layout factor could be considered later; it is not
silently combined with this screen. enc_stage64 reduces code but retains
256 VGPR with nearly triple LDS, so it is weaker than the selected two.

dec_up64 reduces allocated VGPR16 and executable45.4%. Final code contains
v_add_co_u32/v_addc_co_u32 pairs229→60, global_load_dwordx4 279→218,
ds_read_b64 80→84 and barrier signals5→6. dec_dfn64 code shrinks39.8%,
address-add pairs80→52, global_load_dwordx4 193→183, ds_read_b64 64→65,
barriers3→4. These counts are static sites, not dynamic instruction/traffic
totals: waves double and loop structure differs. In particular WMMA sites
roughly halve for the64-channel cases because each wave handles half the
tokens; this does not mean half as much network matrix work.

## Ready for limited GPU checks

w2_prepare_shortlist.py creates two separate experiment directories under
build/aco-runtime-trace/experiment-w2-screen/shortlist/:

- dec_up_64_64_96: first candidate, SPIR-V bbfd504de9eb12ec.
- dec_dfn_64_64: second candidate, SPIR-V8df4481367098c34.

Each has original/shaders and data/shaders plus records/gfx1201/openrr-d3d12-v1.
Both sides carry retained enc96 SPIR-V f3817f3fb0a26545. An assertion
checks only the selected target SPIR-V differs, so the previously retained
enc96 improvement is part of both controls. Strict-ACO record manifests
contain the retained baseline and candidate record, precision global/vector8.

First run existing8warmup/8steady/12DRS correctness against its own control,
record expected absence of arithmetic-policy changes, check nonfinite and
existing Fast limits. Compare first live differences if any; a nonzero Fast
comparison alone is not rejection. Only candidates passing this stage should
receive a small rotated short timing comparison. Long sequences and repeated
end-to-end confirmation belong only to versions selected for retention.

GPU follow-up completed: both shortlisted variants reproduce all20 measured
outputs byte for byte against retained ACO, with no nonfinite values. Fast
keeps the existing quality gates; equality is an observed result.

Two rotated16warmup/48steady/24DRS pairs reject both variants:

| Variant | Steady control→candidate ms | Paired changes | DRS median change |
|---|---:|---|---:|
| dec_up64 single tile | 3.68678→3.72660 | +1.3800%, +0.7810% | +0.5592% |
| dec_dfn64 single tile | 3.75778→3.80716 | +0.6727%, +1.9534% | +2.0886% |

No long test or production integration follows these regressions. Smaller
code and fewer dec_up64 VGPRs did not deliver runtime benefit; LDS increases,
but this experiment does not prove a specific cause of the slowdown. Raw
summaries and output hashes are in the next-optimization evidence directory,
w2-up64/results.json and w2-dfn64/results.json.

## Reproduction and evidence

From the worktree root under WSL, CPU/offline only:

    python3 asi/tests/w2_screen.py
    python3 asi/tests/w2_prepare_shortlist.py
    python3 asi/tests/w2_archive_screen.py

The screen verifies original SPIR-V against the accepted staged hashes;
captures resources and ACO IR for both bodies. Shortlist capture uses dual
address sentinels through build_aco_records.py. compiler-evidence.zip keeps
IR, capture bodies/layouts, SPIR-V, logs and the selected two record files;
compiler-evidence.json records SHA256 for every member. No large readbacks
are generated. All changes remain uncommitted.

Reference: Mochi CLAUDE.md's documented shape-dependent tile/occupancy
tradeoffs and requirement to inspect emitted VGPR/waves before measuring.
The implementations are existing RR shader bodies; no NR shader code or
weight packing is copied.
