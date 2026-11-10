# ACO review follow-up — 2026-11-10

Implementation work continues in the same uncommitted independent worktree.
The independent review report is the basis for this follow-up. Operator tuning
remains paused. The user requested a stop before performance comparisons while
other machine workloads are active. No new timing conclusion is reported here;
future performance runs require the user to confirm the machine is available.

## Changes implemented

- Diagnostic network mode loads the runtime's base _f kernels, flow variants,
  and _lt variants, enables history sampling, and selects matching internal
  clear_range on each compiler switch. Logs distinguish runtime-network from
  base-isolated mode (OPENRR_ACO_ISOLATED=1). This covers the network configuration;
  it does not claim that the Engine harness exercises every texture-ingest or
  direct output path of the runtime API. The runtime bench covers those paths.
- Dispatch comparison resolves _lt names in the lite kernel set and includes
  ten frames with DRS transitions. Independent-engine snapshot mode removes the
  previous 64 KiB truncation and fails on differences. Internal aliases without
  a resource handle are still outside that snapshot view; dispatch comparison
  traces root allocations through pushed addresses instead. These updated
  diagnostic branches still need their GPU validation run.
- Benchmark option --parity-all-frames 1 saves and compares each measured steady
  and DRS output. JSON labels coverage as all_measured_frames or phase_last_frame.
  Readbacks are outside timestamp intervals but affect scheduling, so this is a
  correctness run; throughput must be measured separately without readbacks.
- Mesa capture v4 uses the reference project's source-level sentinel strategy.
  ACO assembler substitutes the sentinel operand with m0, reports substitutions,
  m0 writes, constant data and PC-relative instructions. Counters are per-program.
  The producer requires identical code/config/layout from two different sentinels.
  The old 40-instruction disassembly heuristic is removed.
- Floating precision and cooperative-matrix loads are independent capture options.
  native/local/global precision and scalar/vector8 load policies are in the
  fingerprint and manifest. The package build explicitly retains global/vector8
  pending review; production records were not silently replaced by local policy.

## CPU and build checks

- Private Mesa v4 capture library and Windows diagnostic/bench tools compiled.
- Eight CPU address-validation tests passed, including mismatched code/config,
  missing/duplicate reports, m0 writes, constant data and PC-relative rejection.
- Five record sets captured the same 93 shaders from the stage, with no failures.
  This includes diagnostic stage shaders, but not the separate occupancy/B2
  directories; it is a compiler comparison set, not final package coverage.
- Every generated record parsed with the CPU adapter suite (400 prologue cases,
  ten malformed-record checks). No GPU speed test is needed for these results.

| Record comparison | Identical records | Different records |
|---|---:|---:|
| global/vector8 vs local/vector8 | 68 | 25 |
| global/scalar vs local/scalar | 68 | 25 |
| local/scalar vs native/scalar | 12 | 81 |
| global/vector8 vs global/scalar | 39 | 54 |

For enc_stage_96_128_128_f, global/vector8 and local/vector8 are identical at
256 VGPR, 8 B/thread scratch and 39,048 executable bytes. global/scalar and
local/scalar are identical at 256 VGPR, 24 B/thread scratch and 56,200 bytes.
The lite scalar version uses 32 B/thread scratch. This demonstrates compiler-side
resource differences before PAL import. It does not quantify their performance.
Full per-record differences are in build/aco-review-v4/compiler-comparison.json.

## PAL metadata audit

AMD official PAL source tree examined:
https://github.com/GPUOpen-Drivers/pal/tree/c5e800072a32f68b6ccc4422936d96167c6e0728

gfx12PipelineChunkCs.cpp, BuildRegisters (around lines 354–415):

- VGPR/LDS and floating mode come from stage metadata; scratch enable and user
  SGPR count come from stage metadata, with threadgroup fields from compute metadata.
- SHARED_VGPR_CNT comes from .shared_vgpr_cnt. Current adapter rejects nonzero
  low rsrc3 bits, so its supported records require zero shared VGPRs.
- IMAGE_OP is forced to 1 for PWS+ synchronization. The inherited .image_op=false
  field alone therefore does not prove the final register is false.
- INST_PREF_SIZE is computed from the entry symbol size by GetShaderPrefetchSize.
  The adapter already updates that symbol size to prologue + executable bytes.
  Copying Linux's raw rsrc3 prefetch bits is not required by this public PAL path.
- gfx12ComputePipeline.cpp CalcScratchMemSize (around lines 172–192) treats
  .scratch_memory_size as per-thread bytes and scales Wave64 relative to Wave32.
  The current wave32 metadata is consistent with this rule.

This closes the public-source mapping for the two suspected fields without
guessing metadata keys. It is not proof that Windows driver 26.10.1 uses this
exact PAL revision, nor an observation of its final programmed registers. No
same-machine-code metadata A/B was run, and no field was changed speculatively.

## GPU observations before the user's pause

The revised profile ran with the old v3 record set and logs confirmed _lt,
_fl_lt and sampler enabled. Its timings are excluded because other workloads
were active. It is only execution-coverage evidence.

One v4 global/vector8 correctness run completed before the pause, using eight
steady and twelve DRS frames with every output captured. All frames passed the
existing Fast gate, with no non-finite values. DRS frames 5/6/7 had max ULP
23/35/39 while the final DRS frame was exact. This directly confirms why a last
frame must not represent the entire DRS phase. No timing from this run is used.

## Next validation boundary

GPU testing is stopped. Still required: actual Fast first-divergence diagnosis,
all-frame local/native precision checks, v3/v4 address-adaptation output parity,
and repeated isolated performance runs across policies. Before measuring speed,
stop and obtain the user's confirmation that competing tasks are paused.
Do not claim a performance cause or choose a new shipping precision policy
from CPU record differences alone.
