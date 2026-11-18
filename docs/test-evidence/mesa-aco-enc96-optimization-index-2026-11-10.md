# enc96 ACO optimization review index — 2026-11-10

Checkpoint ba817e3 remains the immutable pre-optimization reference.
All subsequent work is uncommitted on codex/mesa-aco. Global/vector8,
all-ACO Windows D3D12/PAL and existing precision gates are unchanged.
No accepted LLPC/ACO baseline table was rerun.

## Retained result

enc_stage_96_128_128_fl_lt now uses the existing one-token-tile-per-wave
body, preserving the baseline W2 DFN reciprocal policy. VGPR 256→240,
scratch 8→0 B/thread, LDS 12288→19456 B/group, threads64→128.
Six rotated 48/240/64 runtime pairs: steady 3.76140→3.73490 ms
(-0.705%, all six improve); DRS3.54492→3.52402 ms (-0.590%, four
improve, two regress). This is a small runtime gain on the measured
1080p workload, not a resolution of the full LLPC/ACO gap.

All292 extended correctness outputs equal original ACO byte for byte.
The accepted20-frame LLPC/ACO outputs and frame6 intermediate case remain
unchanged. Full frame139 trace confirms the corrected candidate introduces
no difference at the location where the first candidate diverged.
See mesa-aco-enc96-wave-tiles-experiment-2026-11-10.md and its adjacent
corrected/ and enc96-wave-rcp-long-correctness/ evidence directories.

## Single-factor experiments

Each starts independently from checkpoint shader code, not from the
previous experiment. Timing percentages below are local paired runtime
comparisons, not values compared across different time windows.

| Factor | Machine-code result | Runtime steady / DRS time change | Decision and report |
|---|---|---|---|
| Merge LDS integer address simplification | scratch8→0, VGPR256 unchanged | -0.247% / +0.102%, mixed pairs | Reject; mesa-aco-enc96-address-experiment-2026-11-10.md |
| MLP hidden slice32→16 | executable39544→36888B, scratch8 and VGPR256 | +0.074% / -0.224% | Reject; mesa-aco-enc96-mlp16-experiment-2026-11-10.md |
| Serialize independent Q/V chains | eight concurrent FP32 chains→four; VGPR256/scratch8 | +0.078% / +0.955% | Reject; mesa-aco-enc96-qv-serial-experiment-2026-11-10.md |
| Rebase Q/V BDA once per head | scratch8→16, VGPR256 | GPU not run | Reject at code inspection; mesa-aco-enc96-qv-rebase-experiment-2026-11-10.md |
| Delay residual FP16 decoding | scratch8→0, moves decrease, loads unchanged | +0.337% / -0.583% | Reject; mesa-aco-enc96-residual-experiment-2026-11-10.md |
| Use existing tiled output weights, generic offset | extra SALU, executable39944B | GPU not run | Reject at code inspection; projection-tiled-codegate evidence |
| Same tiled weights, direct tile offset | executable39560B, VGPR256/scratch8 | +0.187% / -0.245% | Reject; mesa-aco-enc96-projection-tile-experiment-2026-11-10.md |
| One token tile/wave, unmodified transposed body | VGPR240/scratch0 | initial -1.069% / -0.373%; confirmation -0.763% / -0.414% | Superseded: frame139 exposed DFN reciprocal policy mismatch |
| One token tile/wave with original DFN policy | VGPR240/scratch0, executable38028B | -0.705% / -0.590% | Retain; wave-tiles report |

The old rejected decoder projection-prefetch experiment was not retried.
The output-weight experiments above alter packing addresses, not prefetch
depth. Rejected candidate source copies and records are isolated in build/;
their reports, patches, resources and small raw results are preserved here.
Compiler IR for all enc96 variants is in the 2.1MB compiler-evidence.zip
with per-member hashes, avoiding dependence on large disposable readbacks.

## What was learned and next question

Actual spills were long-lived integer address components, not spilled
matrix accumulators. Removing just those spills did not establish runtime
benefit. W2's two-token live state is a more useful shape-specific change:
the smaller per-wave tile reduces allocation and brings a measured runtime
gain despite more LDS and waves. This follows Mochi's practice of measuring
tile/resource tradeoffs per shape; no NR shader code was copied.

The first tiling candidate also demonstrated why short correctness cases
alone are insufficient: its existing transposed DFN used rcp_rn where W2
uses SIG_RCP under LT_HM. The divergence appeared only at Engine139.
Restoring the existing policy eliminated all measured new differences.
No threshold was widened and no lower precision was introduced.

Next, inspect dec_dfn_96_128_fl_lt's actual W2 register live state and
whether the same tiling factor has a viable LDS/dispatch tradeoff. It has
a decoder DFN head, so first audit its reciprocal and interpolation rules
against the alternative body; do not copy enc96's result mechanically.
Only after generated-code inspection should a new isolated candidate be
tested. No decoder optimization is included in this milestone.

Native same-GPU Linux RADV remains unavailable: WSL exposes dxg/vgem,
not an amdgpu render device. Offline noop capture is compiler evidence;
all timing reported here is Windows D3D12/PAL. No Linux timing is claimed.

## Review storage

Small enc96 evidence directories total about 6.8MiB, including compressed
compiler IR, code, records, manifests, raw summaries and logs. Large GPU
readbacks retained for review total34.29GiB: two long correctness sequences
account for18.04GiB, the rest are intermediate traces and short controls.
The wave-tiles evidence directory's readback-inventory.json lists every run
and byte count. No further readbacks are needed for this completed milestone.
After review, failed variants and the two long sequences can be reclaimed
using the archived hashes/summaries; preserve original baseline traces and
the selected corrected frame6/frame139 cases. C: has about42.5GiB free at
completion. No cleanup of these new review readbacks is included here.
