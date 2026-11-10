# Common FC1 address calculation screen — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


Two isolated candidates pass the offline machine-code gate: the FC1 tiled
weight addresses of enc_stage_128_128_160_fl_lt and dec_dfn_96_128_fl_lt.
No GPU correctness or timing is claimed here. Production sources remain
unchanged. Both candidate data folders retain the already validated enc96
single-tile shader f3817f3fb0a26545 and its record, so their controls must use
the same current retained enc96 baseline rather than the staged old W2 shader.

## Factor and evidence

FC1 knows its logical row j and channel tile n, but passes j*C+n*16 through
WT_FC1, which reconstructs row/tile by quotient and remainder:

    tiled = HID*C + floor(off/(16*C))*16*C + floor((off%C)/16)*256

For j divisible by16 and 0<=n<C/16 this is exactly:

    tiled = HID*C + j*C + n*256

The candidate passes the original row and tile directly via FC1_TILE.
Its non-tiled definition is j*C+n*16, retaining the WTILE=0 contract too.
The generic WT macro and FC2/output/merge addresses are unchanged. There
is no pointer rebasing, descriptor change, matrix tile change or weight
packing change. The previous unsuccessful Q/V BDA-rebase experiment is
not repeated.

Actual final HW IR in the dec96 baseline contains repeated sequences of
s_mul_i32, s_mul_hi_u32, s_lshr_b32, s_mul_i32, s_sub_i32 around FC1
weight loads. After factorization these scalar quotient/remainder
sequences decrease. enc128 eliminates bitfield/shift address recovery
instead. IR before scheduling, after spill and final HW lowering is in
the adjacent compiler-evidence.zip for both sides.

| Kernel | Baseline SPIR-V FNV | Candidate SPIR-V FNV | VGPR | Scratch B/thread | Executable bytes |
|---|---|---|---:|---:|---:|
| enc96 current single tile | f3817f3fb0a26545 | 90a489ca4d3d9fbd | 240→240 | 0→0 | 38028→38028 |
| enc128 flow/lite | 5f78c6a07f1a5ec7 | e39c40884a3aad9b | 240→240 | 0→0 | 37228→36652 |
| dec96 DFN flow/lite | 9b09c8e4e1c7b892 | 607b418f7ed5346e | 256→256 | 0→0 | 27560→26488 |

All three retain108 SGPRs. LDS/workgroup/wave are unchanged:
enc96 19456B/128/32; enc12821504B/128/32; dec969728B/64/32.
enc96's captured code bodies are byte-identical despite different SPIR-V;
discard that candidate before GPU. enc128 and dec96 are worth separate
short correctness/timing tests, with dec96 first because its scalar
address chain improvement is larger.

| Static final-HW opcode | enc128 original→candidate | dec96 original→candidate |
|---|---:|---:|
| s_add_u32 | 222→192 | 120→100 |
| s_mul_hi_u32 | unchanged | 26→14 |
| s_mul_i32 | unchanged | 63→39 |
| s_lshr_b32 | unchanged | 76→40 |
| s_lshl_b32 | 76→60 | 31→19 |
| s_bfe_u32 | 65→49 | unchanged |
| s_and_b32 | 81→65 | unchanged |
| s_sub_i32 | unchanged | 31→19 |
| s_waitcnt_depctr | 306→260 | 444→284 |

All WMMA, global load/store and LDS opcode counts are unchanged on both
selected kernels. dec96 has extra moves (v_dual_mov113→114,
v_mov25→29), v_perm213→211 and s_delay174→180; fewer scalar instructions
does not guarantee runtime benefit. Static counts are not executed totals.
SGPR metadata108 includes the existing ABI allocation; this screen does
not claim a measured residency change.

## Address proof and numerical expectation

The factor is integer-only. j, j+16/32/48 and jt=j+16*hf are aligned to16;
the expansion's channel tile n is in[0,C/16). As no intermediate offset
wraps for these network dimensions, floor(off/(16*C))=j/16 and
floor((off%C)/16)=n. CPU checks enumerate all880 combinations of every
aligned FC1 row and tile for C32/64/96/128/160, matching both formulas.
Offsets are half-element indices and the existing buffer type still
converts them to bytes. Buffer base, tail tiled copy, strides and bounds
are unchanged. Current selected shaders use WTILE=1.

No numerical change is intended: identical bytes enter the same cooperative
matrix operations in the same K order, with identical FP8/FP16/FP32
boundaries, GELU, reciprocal and residual expressions. The existing
enc96 reciprocal correction is retained independently. Fast correctness
should use the established quality and nonfinite gates and compare the
existing first-divergence case; bit equality is useful evidence rather
than an imposed universal Fast requirement. Any nonzero candidate change
still needs explanation because this factor has no intended FP change.

Mochi's guidance to inspect invariant fragment/lane offsets and generated
code motivates this factor. No NR source or layout is copied; RR's
existing tiled-tail layout remains the contract. Descriptor-backed NR
address behavior is not assumed to transfer to RR's BDA buffers.

## Reproduce and short-test inputs

WSL CPU/offline capture from the worktree root:

    python3 asi/tests/address_screen_build.py
    python3 asi/tests/address_screen_archive.py

Use current retained control data:

    build/aco-runtime-trace/experiment-enc96-wave-rcp/data

dec96 candidate data and records:

    build/aco-runtime-trace/experiment-address-screen/dec_dfn_96_128/candidate/data
    build/aco-runtime-trace/experiment-address-screen/dec_dfn_96_128/candidate/records/gfx1201/openrr-d3d12-v1

enc128 candidate data and records:

    build/aco-runtime-trace/experiment-address-screen/enc_stage_128_128_160/candidate/data
    build/aco-runtime-trace/experiment-address-screen/enc_stage_128_128_160/candidate/records/gfx1201/openrr-d3d12-v1

Each folder differs from the retained control in exactly its target SPIR-V.
Each merged record set contains original records, the candidate record and
the current retained enc96 record, so one ONR_ACO_DIR can serve both sides.
Use strict ACO versus strict ACO and separate correctness from short timing.
Do not stack the two candidates for the first tests. Short tests decide
whether to invest in long sequence and repeated end-to-end validation.

Small review data: adjacent screen.json contains exact SPIR-V/ACO record
hashes, resource fields, all static opcode counts and ready-to-run paths;
source-change.patch is the isolated factor; compiler-evidence.zip is
1,393,773bytes with per-member SHA256 in archive.json. Python compile
checks pass. WSL noop capture supplies code evidence, not GPU timing.

## Runtime decision and scoped integration

Both shortlisted address variants reproduce all20 short-correctness outputs
byte for byte, no nonfinite. enc128 short steady pairs are -0.892%/+0.381%,
so it is deferred. dec96 short pairs are -0.541%/-0.444%; only it receives
292-frame correctness and two independent six-pair48/240/64 timing batches.
All292 outputs remain byte-identical. The batches show steady -0.260% and
-0.514% (9/12 pairs improve), DRS -0.847% and -1.087% (10/12 improve).
These are small runtime benefits with fluctuation; every pair is archived.

Production enables FC1_ROW_TILE only for dec_dfn_96_128_fl_lt under the
tested precision defines. FC1_TILE defaults to the old expression for other
variants. Full runtime shader build90/90 matches the tested data package;
source-verification.json additionally checks20 selected variants. Tested
SPIR-V607b418f7ed5346e and ACO code38ac98d35509933f remain the identity.
Legacy and unrelated Fast SPIR-V stay unchanged. The saved LLPC/ACO frame6
case reproduces all20 outputs and69 readbacks per compiler, including the
accepted first nonzero difference. Traced/untraced outputs match.

See ../mesa-aco-next-optimization-2026-11-10.md for full results, ranges,
cleanup inventories, paths and reproduction. Source changes remain
uncommitted; no staged/release package is replaced.
