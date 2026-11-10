# ACO isolated GPU validation — 2026-11-10

The user confirmed the machine was available before these GPU runs. All runs
were sequential; operator code was not tuned. The same review ASI and SPIR-V
were used across independently captured record sets. Compiler values were
explicitly LLPC=1 and ACO=3. Original package ASI/records remain unchanged.
Changes are uncommitted in codex/mesa-aco.

## Controlled performance comparison

1280x720 -> 1920x1080, Fast, ONR_RR_FLOW=1 (logs confirm row flags), normal
runtime direct textures and history sampler. Each run: 24 warmup frames,
48 steady frames, 24 DRS frames. Three rounds changed policy order. LLPC ran
before ACO inside each process. No per-frame readbacks in these speed runs.
Values below are medians of the three per-run medians.

| Precision / matrix loads | LLPC steady ms | ACO steady ms | ACO range ms | Increase | LLPC / ACO DRS ms |
|---|---:|---:|---:|---:|---:|
| global / vector8 | 3.12740 | 3.72388 | 3.69776–3.73712 | 19.1% | 3.01552 / 3.54604 |
| local / vector8 | 3.11960 | 3.72648 | 3.69264–3.72952 | 19.5% | 3.02160 / 3.54412 |
| global / scalar | 3.12528 | 5.01828 | 5.01464–5.02336 | 60.6% | 2.98684 / 4.77268 |
| local / scalar | 3.13472 | 5.01560 | 5.00760–5.03168 | 60.0% | 2.98464 / 4.76552 |
| native / scalar | 3.13676 | 5.02196 | 5.01288–5.02848 | 60.1% | 2.97756 / 4.81036 |

The global-exact setting is not the dominant explanation of this measured
regression: global/local timings overlap, and native/scalar remains about 60%
slower than LLPC here. The vector-load patch materially improves this network
relative to its scalar counterpart, while leaving about 19% regression.
This does not establish the optimal precision policy or prove individual
instructions responsible for the remaining cost. "native" means native
floating/matrix optimization rules within the required capture/ABI patch,
not an unmodified RADV driver running the workload.

The v4 global/vector8 code hashes match all 93 corresponding v3 records.
Thus the stronger source-level address validation did not change those bodies.

## Per-frame output checks

Separate correctness runs used eight warmup, eight steady and twelve DRS frames,
capturing all 20 measured outputs. Readbacks perturb scheduling, so their
timings are not used above. Existing thresholds were unchanged.

| Fast policy | Tested / passed frames | Maximum ULP across frames | Minimum PSNR_01 dB |
|---|---:|---:|---:|
| global/vector8 | 20 / 20 | 60 | 60.1693 |
| global/scalar | 20 / 20 | 60 | 60.1693 |
| local/vector8 | 20 / 20 | 57 | 59.9572 |
| local/scalar | 20 / 20 | 57 | 59.9572 |
| native/scalar | 20 / 20 | 64 | 59.0174 |

All have zero non-finite values. Passing Fast thresholds does not establish
bit equality. Additional Legacy checks (same 20-frame coverage):

- global/vector8: every frame exact, maximum ULP 0.
- local/vector8: strict gate failed in ten frames, maximum ULP 43.
- native/scalar: strict gate failed; steady final-frame maximum ULP 42.

Therefore local/native cannot replace the shipping precision policy merely
because their Fast checks pass. The blanket exact scope still deserves a
targeted replacement, but it must preserve the required rounding semantics.

## Runtime-path and first-divergence checks

The diagnostic now loads _lt and _fl_lt variants, enables sampling/flow, and
reinstalls matching internal clear_range when switching compiler. Logs identify
the actual SPIR-V names. Its profile is a network harness, not a substitute for
the runtime API texture and output path.

Same-address per-dispatch comparison restores LLPC inputs before each ACO
dispatch, compares whole pushed root allocations, and restores the LLPC output
before continuing. Ten frames include DRS at frames 4 and 8:

- global/vector8 Fast: 239 dispatches, 22 differing bytes, only dec_head_32_64_f_lt.
  The first saved difference contains four FP16 +0 versus -0 values. Other
  dispatches compare exactly on those restored inputs.
- With varying checker/noise colors, depth and motion: 239 dispatches, two
  differing bytes, again in the head. This still does not reproduce runtime
  benchmark's nonzero output differences.
- local/vector8 Legacy: 239 dispatches, 29 differing bytes, only the head on
  these harness inputs. This does not override the failing runtime Legacy gate.
- Independent LLPC/ACO engines on the diagnostic input, ten frames with full
  snapshots: final outputs and history buffers equal; only head scratch aliases
  within attr differ at signed-zero positions. The diagnostic correctly returns
  failure for any byte difference rather than printing a false pass.

Runtime all-frame checks with direct textures disabled, history sampler disabled,
and both disabled still reproduce the Fast differences (same maximum ULP 60
and minimum PSNR 60.1693 in this test). Logs confirm those switches took effect.
They are diagnostic controls, not production fallback choices. Consequently
neither switch explains the observed difference by itself. Inputs/history and
runtime-specific execution still need closer alignment with the harness before
claiming its first nonzero divergence has been found.

## Actual Fast network profile

Single diagnostic profile, 8 warmup + 16 samples with per-dispatch timestamps.
These timestamps serialize the network and should locate costs, not replace
end-to-end throughput measurements. Largest positive deltas (ms):

| Kernel | LLPC | ACO | Delta |
|---|---:|---:|---:|
| dec_dfn_96_128_fl_lt | 0.085888 | 0.147037 | 0.061149 |
| enc_stage_128_128_160_fl_lt | 0.041810 | 0.092050 | 0.050240 |
| enc_stage_96_128_128_fl_lt | 0.172233 | 0.217023 | 0.044790 |
| dec_up_160_256_160_fl_lt | 0.053485 | 0.093982 | 0.040497 |
| enc0_stage_32_64_f_lt | 0.373188 | 0.413355 | 0.040167 |
| dec_head_32_64_f_lt | 0.310140 | 0.346955 | 0.036815 |
| dec_up_128_128_160_fl_lt | 0.085453 | 0.121243 | 0.035790 |
| dec_dfn_128_128_fl_lt | 0.047750 | 0.080545 | 0.032795 |

The remaining cost is spread across multiple kernels. This is not evidence of
a single miswired PAL argument. No speculative register changes were made.

## Evidence and next boundary

JSON summaries, full profile/dispatch logs and artifact hashes are copied into
mesa-aco-idle-validation-2026-11-10/ beside this document. Raw per-frame images,
record sets and individual speed logs remain under build/aco-review-v4/.
The v4 ABI GPU probe passed all 61 push dwords, spill/grid, original lane-varying
selftest, BDA fill, and sampled descriptor slots 0/3/7. git diff --check passed.

The controlled comparison requested by the independent review is complete for
this 1080p workload. No new precision policy was promoted, no LLPC fallback was
introduced, and operator optimization remains paused. Remaining work is to
reproduce the runtime's nonzero precision divergence with matching inputs and
history in the layer diagnostic, narrow exact constraints while retaining
Legacy exactness, and only then investigate targeted code generation changes.
This is not completion of the full three-resolution release validation matrix.
