# Mesa ACO review checkpoint — 2026-11-09

This is an intermediate review checkpoint, not a release acceptance report.
Further operator tuning is paused at the user's request. D3D12 requires ACO
for every kernel; LLPC remains an explicitly selected diagnostic reference.

## Checkout and implementation

- Worktree: `C:/Users/Kenny/.codex/worktrees/mesa-aco-0101/OpenRR`
- Branch: `codex/mesa-aco`; base: `a2333b3`; changes are uncommitted, including new files.
- Hardware: RX 9070 XT / gfx1201; Windows driver 26.10.1.
- Offline compiler: Mesa 26.2.4, commit `96cb43121031992b85767f9c1be8f3f48e22b1d2`.
- Current record directory contains 129 records, including diagnostic shaders.

The adapter replaces a wave32 clear_range PAL container with ACO code, its
resource metadata, and an argument prologue. Effective push data is 244 bytes,
mapped to PAL entries 4..64; descriptor pointer is entry 65; continuous push
pointer is spill base +16. Combined sampler tables use RADV's 64-byte stride
and sampler offset 48, separately for each recording slot.

Primary review entry points:

- `gpu/src/rr_pal_binary.hpp`: record parser, PAL ELF/MessagePack adapter, argument prologue.
- `gpu/src/d3d12.cpp`: record identity checks, pipeline import, descriptor tables and binding.
- `asi/build/build_aco_records.py`: address32 adaptation, record capture and manifests.
- `asi/build/mesa/prepare_capture.py` and `capture.patch`: changes to pinned Mesa.
- `runtime/src/runtime.cpp` and `runtime/include/opennr_rr_api.h`: compiler/backend selection.
- `gpu/shaders/enc0_attr.comp`: direct packing of FP16 bit patterns.
- `asi/tests/test_pal_binary.cpp`, `test_asi_units.cpp`, `aco_compare.hpp`: validation.

## Confirmed problems fixed

1. Divergent push-constant offsets were lowered to scalar loads/readfirstlane.
   The Mesa patch preserves divergent offsets through global loads. The original
   selftest remains intact and now executes on ACO.
2. Identical attribute inputs could produce an incorrect exposure field in the
   LLPC result, equal to another attribute's half value. Direct FP16 bit packing
   avoids the implicit widening/reconversion. Legacy output parity is now exact
   in the short benchmark. This is observed compiler-sensitive corruption; a
   minimized upstream compiler reproducer has not been completed.
3. Default compiler selection happened before resolving ONR_RR_BACKEND. Backend
   resolution now precedes compiler selection. Tests cover old API sizes 64/80
   and current size 88 with the environment selecting native Vulkan.
4. A partial optional backend field could be read based on size > V1. It now
   requires the complete field; invalid backend choices are rejected.
5. Negative argument positions other than -1 and non-dword push sizes could be
   accepted. Parser now rejects them, plus inconsistent scratch enable/size.
6. Diagnostic compiler switching retained the previous internal clear_range
   pipeline. Switching now retires that reference and resets the internal kernel.
7. ONR_ACO_DIR previously also dictated the container SPIR-V location. The loader
   now first uses clear_range beside the requested shader, then package layout.

The initial scratch audit guard rejected every nonzero scratch record. Full
build/test exposed four enc_stage_96_128_128 variants with 8 bytes/thread.
Source inspection confirmed gfx1201 implicit hardware scratch addressing:
radv_shader_args.c adds scratch_offset only below GFX11; aco_spill.cpp emits
scratch load/store without a resource. The adapter passes scratch enable and
per-thread size to PAL and checks their consistency. The blanket guard was
removed, and the complete GPU suite then passed with these records.

SGPR count 108 is consistent with Mesa ac_gpu_info.c's fixed GFX10+ allocation,
including VCC in s106..107. The template's sgpr_limit 106 refers to general SGPRs;
this observation alone is not evidence of an allocation error.

## Validation at this checkpoint

- Complete build of GPU/runtime/ASI, shaders, tests and records succeeded.
- CPU: 400 gfx1201 prologue simulations and 10 malformed-record cases passed;
  every current record parsed. The interpreter checks the emitted scalar subset,
  not GPU timing hazards or arbitrary instructions.
- Full ASI unit suite passed, including 1080p/4K engine checks, occupancy and B2
  comparisons, pooling race negative control, recording retirement, Vulkan bridge
  and Vulkan-host paths. Those checks are not all LLPC/ACO parity comparisons.
- Dedicated GPU probes passed: all 61 push dwords, spill, grid count, original
  lane-varying selftest, BDA fill and sampler array slots 0/3/7.
- API compatibility/env tests passed for sizes 64, 80, 88.
- Short sequential same-process A/B uses compiler 1 versus 3, 1280x720 to
  1920x1080, 8 warmup frames, 24 steady frames and 12 DRS frames.

| Mode / phase | LLPC median ms | ACO median ms | ACO time increase | Output |
|---|---:|---:|---:|---|
| Legacy steady | 4.94732 | 5.93832 | 20.0% | 100% exact, max ULP 0 |
| Legacy DRS | 5.71152 | 8.96560 | 57.0% | 100% exact, max ULP 0 |
| Fast steady | 4.68780 | 5.61436 | 19.8% | PSNR 59.6216 dB, max ULP 49 |
| Fast DRS | 5.16024 | 6.52604 | 26.5% | 100% exact, max ULP 0 |

All compared outputs have zero non-finite values. Fast steady exact fraction
is 48.972%, p99 absolute error 0.00292969, maximum absolute error 0.0112305;
it passes the existing Fast gate, but its remaining differences require review.
These are single short runs. Earlier absolute timings differed materially; do
not treat this as a stable throughput claim or as completion of the three-run
1080p/1440p/4K matrix. No thresholds were relaxed and no fallback selected.

## Open review items and boundaries

- The cooperative-matrix vector-load patch (up to eight consecutive elements)
  remains experimental. Review element widths, vector type legality, alignment,
  dereference semantics and the global/column-major applicability guard.
- NIR currently marks all supported floating ALU controls exact, in addition to
  strengthening weak FMA. Review the required precision scope and remaining
  Fast differences before any precision/performance adjustment.
- The previous basic-kernel dispatch comparison differed only at 22 FP16 signed
  zero bytes in dec_head across two frames. It does not cover Fast lite or all
  resolutions; no signed-zero normalization was added to the head.
- Address32 adaptation inspects llvm-objdump-21 output, whose gfx1201 WMMA
  decoding is incomplete. Its checks do not prove complete instruction coverage.
- PAL rsrc3 instruction prefetch and inherited image_op metadata still need a
  schema/source audit. They were not changed based on guesses.
- Temporary diagnostic hooks and buffer tracking remain in D3dDevice/Engine,
  along with diagnostic shader files in the local stage. Clean these before
  release; this worktree is intentionally preserved for review.
- Scratch received source and network-output validation, but no standalone
  synthetic stress test varying scratch size has been added.
- Formal Top-8 resource/timing comparison and the repeated resolution matrix
  remain unfinished. Current performance is a debugging target, not accepted.

## Evidence and reproduction

Review logs and summary JSON are copied into `mesa-aco-review-2026-11-09/` beside
this document. Full compiler logs and older experiments remain in
`build/aco-tools/`. Build outputs are under `build/asi/`; the ASI is
`build/asi/stage/plugins/openrr-amd.asi`.
The final rebuilt ASI SHA256 is
`BE66500153EC0EF6403709C0A3A9B5DA1EF8616821D9EA12F8F72FAB9005797A`.
Final rebuild ABI and API probes also passed; their logs are archived separately.

Build under WSL with `bash asi/build/build_asi.sh`. For CPU checks, compile
`asi/tests/test_pal_binary.cpp` with C++20 and `-Igpu/src`, then pass record paths.
Run `build/asi/test_asi_units.exe` for the complete suite. Set OPENRR_ACO_PROBE=1
for ABI checks or OPENRR_ACO_API_PROBE=1 for API/backend checks. When loading
shaders from diagnostic/variant directories, set ONR_ACO_DIR to the absolute
`build/asi/stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1` directory.
