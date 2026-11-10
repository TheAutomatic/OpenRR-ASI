# Runtime precision baseline and first divergence — 2026-11-10

Subsequent performance baseline and the first isolated optimization experiment:
[projection-prefetch results](mesa-aco-prefetch-experiment-2026-11-10.md). The
diagnostic findings below remain unchanged; the later variant was rejected.

Worktree: codex/mesa-aco. All changes remain uncommitted. Original staged
shaders, records and release ASI were not overwritten. No network arithmetic,
precision policy, matrix lowering or operator optimization was changed in this
stage. No performance conclusion is drawn from these correctness runs.

## Result

The actual Windows D3D12/PAL runtime reproduces the existing Fast nonzero
difference. In the phase-aligned sequence, its first differing live network
input attribute is at Engine frame 6, phase8=6, dispatch 3
d3d_enc0_attr_f_lt, pixel (203,378), attribute 9: red from reprojected
DownsampledHistory2. Its 48-byte packed attribute record differs at one bit.
This differs from the earlier synthetic harness's head signed-zero finding.

An independent repeat of original trace6 reproduced **all 69 readbacks byte
for byte for each compiler**, as well as all 20 measured outputs. This is a
saved regression case, not a one-run observation.

Evidence supports a floating-point evaluation-order explanation for this case.
Windows LLPC combines UNORM10 normalization with interpolation weights;
global-exact ACO rounds normalized taps and subsequent arithmetic separately.
Actual compiler representations and CPU simulation explain the observed
two-FP32-ULP bilinear difference and subsequent FP16 midpoint crossing. No PAL
mapping/address-adaptation error has been demonstrated by this case. This is
not an exhaustive audit of every import field and kernel.

## Configuration and gates

- Mesa 26.2.4 commit 96cb43121031992b85767f9c1be8f3f48e22b1d2, capture v4,
  global/vector8, gfx1201. Accepted records reused; no five-policy comparison
  or 465-record hash audit repeated.
- Same diagnostic ASI for explicit LLPC=1 and strict ACO=3. Candidate kernels
  and internal clear_range remain ACO. Mandatory actual runtime logs:
  direct_guides=1, direct_output=1, sampler=1, flow=1.
- UploadPattern and actual texture guides; 1280x720 -> 1920x1080,
  jitter=(0.125,-0.25), motion scale=(1,1), hardware depth, benchmark projection
  and depth sign. 8 warmup, 8 steady, 12 DRS. DRS render rectangles are
  1280x720/1088x612; resets at Engine 16,20,24.
- User confirmed GPU idle and approved four further frame-4..7 traces.
  Subsequent diagnostic repeats kept the same correctness objective. User
  later authorized performance testing without a separate request; no speed
  experiment was performed here.
- Every original trace's 20 measured outputs match its own untraced aligned
  run byte for byte. Trace numbering starts at warmup 0; steady 0 is 8.
  Untraced control does not save warmup outputs: same-frame warmup
  traced/untraced equality is not claimed. Original trace6's independent
  repeat adds a reproducibility check.

Both requested review corrections are implemented. compare_runtime_trace.py
structurally parses dispatches: ignores only aco, compares indirect, groups,
push size and every non-pointer push byte. It checks projection/view matrices,
dimensions, formats, origins, pitches, jitter, depth sign, phase/reset/fresh.
run_runtime_trace_batch.ps1 clears ONR_RR_DIRECT alongside DIRECT_GUIDES and
NO_SAMPLER, and requires actual direct/sampler/flow path logs.

Pointer slots remain explicit. Automatic normalization for independently
allocated addresses is not implemented; unknown push interfaces fail closed
and reports mark address review pending. In all nine original trace pairs,
every push pointer raw value, every named view address/size/root/offset and
sampled descriptor bytes match across compilers. Direct texture COM identities
are independent; slot/origin/format/layout and actual texture readbacks match.
At frame6, guide-table/parameter readbacks also match. Fixed weights/LUT come
from the same validated weights file and allocation path; full immutable-weight
readbacks and exhaustive descriptor normalization are not implemented.
See case-inspection.json and trace-*/ for the manual address-review evidence.

## Runtime results

All runs retain existing quality gates; output nonfinite counts are zero.

| Sequence | Measured frames | Max ULP over frames | Min PSNR_01 dB | Result |
|---|---:|---:|---:|---|
| Fast original readiness/phase | 20 | 60 | 60.1693 | Reproduces accepted original workload |
| Fast explicit phase/fresh alignment | 20 | 44 | 61.4079 | Nonzero difference remains |
| Legacy aligned | 20 | 0 | 999 | Every output bit exact |

Aligned DRS is exact in these 12 frames, with resets every four frames. This
does not generalize to other history/reset schedules. Steady output is not
bit exact. Raw summary timing fields are not speed evidence: correctness
readbacks and tracing perturb scheduling.

| Engine frame | Entry guides/history | First different readback | Final output |
|---:|---|---|---|
| 0 | Paired captured inputs equal; fresh/reset | head tile: 5 FP16 signed zeros | Exact |
| 1 | Equal | head tile: 1 FP16 signed zero | Exact |
| 2 | Equal | None; all 69 equal | Exact |
| 3 | Equal | head tile: 7 FP16 signed zeros | Exact |
| 4 | Equal | None; all 69 equal | Exact |
| 5 | Equal | None; all 69 equal | Exact |
| 6 | Equal, including previous motion/hidden/down/history/exposure | attr: one byte/one bit | Nonzero difference |
| 7 | Previous hidden/down/history already differ | Entry state precedes dispatches | Nonzero difference |
| 8 | Previous hidden/down/history already differ | Entry state precedes dispatches | Nonzero difference |

Head signed zeros occur at half index9 within the 12-half post tile, the
cross-anisotropy t2 field. They have no nonzero output effect in frames0..5
here and are not the demonstrated runtime nonzero source.

Packed pooled-history/down/mvf are not FP16. Initially the classifier matched
pooled-history's suffix too broadly; this was corrected and trace8-diff.json
regenerated. Apparent packed-buffer nonfinites in the superseded CPU report
were classification errors, not GPU NaNs.

## Saved frame6 case

Original SPIR-V hash: 1d6fd9f4cc1c893c; ACO code hash: 573daf074f5a5b8d.
96 VGPR, scratch=0, LDS=0, executable28424 bytes, push236, ACO wave32.
No replacement record is retained as an optimization.

- Attribute buffer byte23234111, invocation484043 -> pixel(203,378), record byte47.
- LLPC0x6e vs ACO0x6f, XOR1: bit0 of pl[1]'s upper byte, attribute9's retained
  half bit7. Input-to-embed quantized half: LLPC0xb200(-0.1875),
  ACO0xb280(-0.203125). These quantized values amplify the small preceding error.
- Frame6 output differs in 551999 nonzero half elements. First element1497749:
  0x323b vs0x323e. Existing quality gate still passes.
- Frame7 enters with1679623 differing sampled-history bytes and889005 hidden
  bytes; propagation is captured directly.
- ds2=720x405; texel origin(114,212); fractions(10,118)/256.
  Four red codes188,188,187,188. Current/previous exposure0.79541015625,
  attribute scale-0.43359375, phase6 LUT noise67.

## Arithmetic evidence and limits

prepare_attr9_trace.py generates an isolated full-attribute diagnostic shader
without editing staged shaders. Target intermediate values are written after
padded ATTR64 records and copied immediately after attr. It is diagnostic only.

The full probe's own untraced control matches all20 measured traced outputs
per compiler. At frame6 its active attribute buffer, final output and captured
non-motion network readbacks equal the original kernel per compiler, preserving
the original single-bit difference. It also changes some reflection-motion
values: first example pixel0,0 channel3, original-3.8743e-6 vs probe6.23465e-5.
This affects DRS frames5..7. It cannot replace the original baseline. Attribute9
uses central motion and pooled history, not reflection motion; those relevant
inputs remain aligned. Do not generalize this probe to other attributes/DRS.

| Target intermediate in full probe | LLPC | ACO |
|---|---|---|
| Bilinear | 0x3e3bbd8a =0.18334022164344788 | 0x3e3bbd8c =0.18334025144577026 |
| Attribute before FP16 | 0xbe476fff =-0.1947631686925888 | 0xbe477001 =-0.1947631984949112 |
| Attribute FP16 | 0xb23b | 0xb23c |
| Half dither | 0xb27e | 0xb280 |
| Retained half | 0xb200 | 0xb280 |

FP16 midpoint is exactly-0.19476318359375, between the two FP32 values.

Actual Windows compiler ISA for original SPIR-V was obtained through
vkGetPipelineExecutableInternalRepresentationsKHR, subgroup unspecified as
in Kernels::subgroup_of for this attribute. This is compiler output, not a
measurement of driver-final compute registers. In the ds2 section of
llpc-ir/d3d_enc0_attr_f_lt.0.txt, push offset0xb4 supplies dimensions. Constants
0x36802008=round_f32(1/(1023*256)),0x3a802008=round_f32(1/1023) build weights:

    wx1 = round_f32(kx * round_f32(1/(1023*256)))
    wx0 = fma(-round_f32(1/(1023*256)), kx, round_f32(1/1023))
    top = round_f32(round_f32(code00*wx0) + round_f32(code10*wx1))
    bot = round_f32(round_f32(code01*wx0) + round_f32(code11*wx1))
    result = round_f32(round_f32(top*(1-fy)) + round_f32(bot*fy))

CPU simulation on saved data yields exactly0x3e3bbd8a, matching full-probe LLPC.
The excerpt and complete driver ISA are archived. Original ACO final hardware
instruction IR keeps per-tap normalization and separate products/adds, yielding
0x3e3bbd8c in CPU replay. It was recaptured offline with pinned noop setup;
the captured code body equals the existing baseline byte for byte. Its final
instruction IR, optimized NIR and post-spill IR are archived in aco-ir/.
No Linux GPU execution or timing is implied by this compilation capture.

This narrows the case to normalization/interpolation evaluation order, rather
than an unproven transcendental difference: bilinear has already diverged
before exp2/log2. The global-exact ACO configuration is part of this baseline;
this does not represent native ACO behavior, and does not reopen global-exact
removal as the main performance hypothesis.

A smaller aco_attr9.comp diagnostic did NOT reproduce the difference: both
compilers produced0x3e3bbd8c and0xb23c on64 identical lanes. The surrounding
optimization context matters. Preserve this failed reduction. A temporary
selected-frame replacement mechanism also changed steady outputs and failed
its gate; it was removed from runtime source. Failed logs remain archived.

Source, original compiler instructions, CPU evaluation and the full probe give
a consistent numerical explanation for this specific divergence. Remaining
work is a minimal reproducer retaining the original compiler transform without
changing reflection math, or bounded replay of original instructions. No
precision-boundary change is proposed just to force Fast bit equality.

## Regression use and next work

Run baseline stage, trace6, compare metadata, captured inputs, the attribute
bit and final quality. Legacy remains exact; Fast retains all existing
per-frame thresholds, finite checks and first-divergence tracking. A candidate
must report movement/addition of first divergence.

    bash asi/build/build_runtime_trace.sh
    python3 asi/tests/test_runtime_trace_compare.py
    powershell -File asi/tests/run_runtime_trace_batch.ps1 -Stage Baseline -Output <new-dir>
    powershell -File asi/tests/run_runtime_trace_batch.ps1 -Stage Trace -TraceFrame 6 -Output <same-dir>
    python3 asi/tests/compare_runtime_trace.py <baseline-trace-6> <candidate-trace-6> --out <diff.json>

Full probe: build_attr9_full.sh, isolated -DataDir/-Records. Use -Stage Control
-TraceName <control> for its untraced control; select it via -ReferenceName.
Do not weaken original-output checks to treat changed diagnostics as baseline.

CPU: ten comparator tests, PowerShell parse and git diff --check passed;
isolated ASI/benchmark/test exe built. Original trace6 repeat passed all20
output hash comparisons and all69 intermediate readbacks per compiler. New
executable/SPIR-V SHA256 values are archived, without repeating old package audit.

No operator/performance variants created. Next performance candidate remains
the documented register-live-range/prefetch investigation in
enc_stage_96_128_128_fl_lt/dec_dfn_96_128_fl_lt, after baseline review. Mochi
source/packing/Windows import analysis and native Linux availability are in
mesa-aco-runtime-diagnostic-preparation-2026-11-10.md. WSL exposes vgem/dxg,
not native same-GPU amdgpu/RADV. Future speed tests use Windows D3D12/PAL,
separate correctness readbacks, rotated order, medians/variation and runtime
end-to-end confirmation before retaining a variant.

## Evidence

Small summaries, frame CSVs, original trace metadata, comparisons, CPU replay,
compiler representations and failed diagnostic logs:
docs/test-evidence/mesa-aco-runtime-diagnostic-2026-11-10/.

Large original buffer readbacks and final outputs:
build/aco-runtime-trace/runs-approved-2026-11-10/.
Original cases are fast-trace-0..8 and fast-original-repeat-trace-6. Directories
named attr9 are diagnostics; failed variants are not original-baseline passes.
