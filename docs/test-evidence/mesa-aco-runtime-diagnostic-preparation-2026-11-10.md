# Runtime precision diagnostic preparation

Preparation record. GPU execution subsequently completed with user approval;
see mesa-aco-runtime-diagnostic-results-2026-11-10.md for results and limits.
Worktree: codex/mesa-aco; all edits remain uncommitted.

CPU validation: isolated diagnostic ASI, benchmark and unit-test executable
compiled successfully. Trace-comparator CPU tests cover signed zero versus
nonzero, equal nonfinite values, size mismatch, compiler-only differences,
groups/indirect mismatches, matrix/guide layout mismatches, non-address push
changes, explicit pending address review, and missing dispatch fields. PowerShell batch parsed without
errors; git diff --check passed. No GPU test result is claimed for this build.

The accepted baseline remains Mesa 26.2.4 commit
96cb43121031992b85767f9c1be8f3f48e22b1d2, capture v4, global/vector8,
Windows D3D12/PAL on gfx1201. No shader arithmetic, precision policy, matrix
lowering or weight packing has been changed in this stage. The prior five-policy
matrix and hash audit have not been rerun.

## What this changes

The older engine harness is not the runtime precision baseline. Its input colors,
normals, albedo, depth and projection do not reproduce UploadPattern and runtime
ingestion. Its two DRS paths now repack varying guides into current-width rows;
this repairs their layout without claiming that their input is benchmark input.

The new diagnostic uses openrr_bench's existing UploadPattern, OnrRrFrame,
runtime Record, direct-guide descriptors, ingest, Engine, sampler and direct
output. It inserts buffer/texture copies on the same host command list. The
benchmark waits its own fence before invoking the diagnostic readback export.
There is no alternate synthetic network execution or resource restoration.

Implemented in runtime/src/aco_runtime_trace.hpp and runtime/src/runtime.cpp:

- Named output views after dispatches, including Fast views that carry only a
  GPU address. Resolve these against actual sized/fixed/history allocations and
  check copy bounds; do not silently omit views lacking their own COM resource.
- Previous motion, hidden, downsampled history, sampled history and exposure at
  the selected frame's entry; attributes, parameter/guide tables, encoder and
  decoder tensors/DFNs/pools, head tile/surfaces and post history/planes.
- Actual rectangular direct texture inputs, stripped of copy row padding;
  ingest destinations and copied texture input buffer; direct output texture.
- Actual SPIR-V path/hash, ACO record path/code hash/rsrc1/scratch/manifest via
  compiler logs. Explicit record directory's manifest is copied beside a trace,
  retaining precision and matrix loading strategy. Baseline records are reused.
- Frame phase, frame modulo eight, fresh/reset/lite state, render extent,
  projection, depth sign, jitter and motion scale; guide format/pitch/origin,
  descriptor slot and resource; sampled descriptor bytes; push bytes and direct
  dispatch group counts. Indirect dispatches are identified, not silently lost.

The CSV logs every building, warmup, steady and DRS call, including Engine phase
before and after the call. The original asynchronous readiness loop is unchanged.
An explicit --align-engine-phase 1 diagnostic resets phase to zero and sets
fresh at the beginning of warmup, after readiness and the host fence. This is
separate from the original-sequence run: compare the original behavior first,
then use deterministic warmup to distinguish initialization/phase effects.

--trace-frame counts from warmup zero. With warmup 8, steady frame zero is trace
8; the DRS sequence is 16..27. Trace only one frame per run. ONR_RR_TRACE_KERNEL
can restrict output copies to one exact SPIR-V stem when full tracing changes
the reproduced behavior.

Copies serialize dispatches, including the flow chain. Trace timing is not
performance evidence. Confirm the traced output against the corresponding
untraced output before attributing differences. Allocation capacity, tensor
padding and temporarily unused head storage may differ without being live
numerical outputs; inspect the active extent and consumer before naming the
first numerical divergence. Parameters/guide tables contain allocation addresses
and must be normalized before byte comparisons. Separate compiler contexts have
independent histories: an entry-state difference precedes the traced dispatch.

asi/tests/compare_runtime_trace.py structurally parses each dispatch and rejects
mismatched phase/reset/lite/frame settings, direct/indirect, groups, push size
and non-address push fields. It also checks both projection/view matrices and
guide/texture format, size, pitch and origin. Explicit pointer-slot schemas come
from scalar-layout push structs, with representative C++ offsetof assertions.
Pointer slots are retained as raw values and listed for resource/view plus
offset review; no heuristic scan or automatic address equivalence is claimed.
The report explicitly marks input equivalence unestablished until this review
is done. It reports raw bytes and separately classifies
FP16 signed zero, nonzero differences and nonfinite values on known FP16
surfaces. Packed attribute/FP8 records remain byte comparisons. It does not infer
that every two bytes of a mixed buffer are an FP16 value.

Build: bash asi/build/build_runtime_trace.sh. Outputs are isolated under
build/aco-runtime-trace: openrr-trace.asi, openrr_bench.exe, test_aco_units.exe.
Existing build/asi objects are read as link dependencies; the script rebuilds GPU
and runtime objects in its own directory. The staged package and archived ASIs
are not overwritten. The diagnostic export is added only to this build's .def.

## Reference implementation findings and RR candidates

Read reference CLAUDE.md; linux/shaders/rdna4/gemm1x1.comp;
linux/shaders/rdna4/pipelines.json; linux/src/core/nr_graph.cpp weight packing;
windows/src/core/nr_pipeline_binary.hpp; windows/src/core/nr_pal_binary.hpp;
windows/build/build_package.sh. No reference code has been copied in this stage.

Windows packaging explicitly builds the Linux shaders into aco-shaders and
looks up records by those hashes. Native Windows shader interfaces provide PAL
containers, with aliases where needed. That project's path uses Windows Vulkan
pipeline binaries; RR imports ELF with CreateComputePipelineFromElf into D3D12.
Both execute ACO machine code on the Windows driver, but their API and descriptor
contracts are not identical. No evidence here challenges ELF import support.

Specific next candidates, after the correctness baseline:

| RR kernel/body | Source observation and Mochi comparison | Single-factor hypothesis and required evidence |
|---|---|---|
| enc_stage_96_128_128_fl_lt, dec_dfn_96_128_fl_lt / W2 | Two subgroups retain two token tiles; W2_DEFER keeps both attention outputs before projection; NT=6 explicitly prefetches p0/p1/q0/q1; MLP has W2_SL 32/64. Mochi documents pipeline depth increasing VGPR and reducing residency without frame benefit. | First try shortening one projection prefetch live range, without changing per-accumulator MMA order; inspect scratch instructions, VGPR allocation, waits and critical path. Shrinking the MLP slice is a separate experiment; compare reduction order before testing. |
| enc_stage_128_128_160_fl_lt / transposed body | Holds input tiles and accumulators through Q/V, attention, output projection and MLP. FP8 Q/V uses BDA, while Mochi hoists fragment base/lane/chunk offsets in descriptor-backed arenas. | Inspect repeated 64-bit address adds and conversions around W8STEP/WSTEP; hoist only a repeated invariant or change one pointer representation. Require actual reduction in address VALU/SGPR dependencies and no added live-range spill. Do not replace BDA wholesale without measurements. |
| dec_up_160_256_160_fl_lt / SPLIT=2 | Eight subgroups split Q and V via hf, stage upsample inputs in LDS, then exchange fragments. Mochi shows tile sizes must match actual dispatch count, and larger register tiles can starve small shapes. | Verify subgroup-uniform hf/pointer selection lowers to scalar control. Only if emitted code has divergent selection/phi copies, test an explicit uniform expression. RR already uses gl_SubgroupID, so adding readfirstlane may compile identically. Never apply it to lane-varying push indices. |
| W2 and transposed output/FC weights | RR already appends 16x16 tiled FP16 copies with with_tiles and WTILE; Q/V also has an exact FP8 copy. Mochi packs adjacent N fragments for 16-byte per-lane loads and binds host layout to shader manifests. | Inspect which RR operand is still strided or loaded twice after vector8. Pair packing is a later host+shader experiment only for that operand. Preserve canonical weights and K order; CPU round-trip packing check and runtime precision gate before speed tests. |

Mochi's raw-u32 loop-carried fragments avoid byte-wise FP8 phi copies, and its
macro expansion avoids dynamically indexed accumulator spills. These are useful
machine-code search targets for RR's Q/V and MLP loops, not proof that RR has
either defect. RR's W2 output prefetch and with_tiles already implement part of
the approach; duplicating them adds no value.

Mochi comments preserve failed experiments: deeper pipelines did not improve
the frame; LDS staging worsened direct-load ViT GEMMs; wide tiles helped large
ViT shapes and regressed smaller projections. Its current manifest also sets
NR_WAVE_UNIFORM=0 on several pipelines. Treat these as shape-specific evidence,
not globally enabled recommendations. No new lower precision/int4 path is in scope.

## Native Linux availability

Read-only environment inspection: one installed WSL2 Ubuntu, kernel
6.18.54.1-microsoft-standard-WSL2. /dev/dxg exists. Both /sys/class/drm/card0 and
renderD128 resolve to devices/faux/vgem. No amdgpu DRM render node is exposed.
radeon_icd.json and libvulkan_radeon are installed, but that does not supply an
amdgpu device. No native same-GPU Linux RADV execution environment has been
established. No Vulkan device benchmark or Linux GPU timing was run.

If a native boot/host becomes available, the minimal proposed cross-platform
test is enc_stage_96_128_128_fl_lt with aligned captured input buffers and row
flags in the same window grid, subgroup32 and local_size64. Use the pinned Mesa,
same patches/global/vector8, identical SPIR-V and weights, exact dispatch input
state and initialized flags. First compare code/config against the captured ACO
record, then output and resource use, then repeated timings. Linux executes via
RADV/amdgpu; Windows executes the same record via D3D12/PAL. Keep Windows runtime
as the product baseline. This proposal still requires environment preparation
and user confirmation; WSL noop is not a substitute.

## Proposed GPU batch requiring approval

Purpose: reproduce the runtime Fast nonzero output difference and establish
where input/history/dispatch first differs. No operator or policy variants, no
performance comparison.

Configuration: 1280x720 -> 1920x1080, flow=1, sampler/direct guides enabled,
global/vector8 records, LLPC explicit reference and strict ACO candidate from
the same new diagnostic ASI. 8 warmup, 8 steady, 12 DRS; DRS uses the original
85% alternating rectangles/reset schedule. Every measured output is read back.

The batch script clears ONR_RR_DIRECT as well as ONR_RR_DIRECT_GUIDES,
ONR_RR_NO_SAMPLER and trace filters. Each paired run requires actual direct
texture/sampler check-passed logs and runtime direct_guides/direct_output/
sampler/flow=1; any logged path change fails the case.

Execution remains gated on a fresh user confirmation that the machine is idle.
Run Stage Baseline first; inspect reproduction and Legacy results before
selecting Stage Trace and TraceFrame. The script no longer chains all four
runs without this inspection. Stage Trace compares every measured output file
against its corresponding untraced aligned run for each compiler; a mismatch
stops interpretation of the intermediate trace.

Four planned paired runs in asi/tests/run_runtime_trace_batch.ps1:

1. Fast, original readiness/phase behavior, trace off.
2. Fast, phase/fresh aligned at warmup, trace off.
3. Legacy control, same alignment, trace off, retain exact requirement.
4. Fast aligned, one full dispatch trace at first steady frame (index 8).

Within the same approved objective, allow up to four further paired traces:
warmup zero if history already differs at first steady entry; first lower-DRS
frame (index 20); the first frame with a differing final output; a restricted
single-kernel trace if full tracing changes output. Choose only those supported
by the initial results. No kernel or precision variants are included.

Scale: initially 224 network frames plus asynchronous readiness calls; at most
448 network frames plus readiness across eight paired runs. Readbacks are large
(hundreds of MB per traced frame); reserve 15 GB of disk space and allow up to
2 GB additional memory for one frame's readbacks. Request an idle GPU for one
10-15 minute diagnostic window. Keep correctness and any future speed run
separate. Necessary diagnostic/build fixes and repeats stay within this batch;
performance or a different experimental objective needs a new proposal.

Decision after execution: if live inputs/history differ, resolve that earlier
state first. If a dispatch has identical inputs but nonzero outputs, save the
case and inspect its shader/ACO/LLPC arithmetic or ABI. If copies remove the
difference, restrict to one boundary and investigate scheduling/aliasing. Do
not identify a PAL parameter, float rule or optimizer defect without that evidence.
