# ACO baseline checkpoint before next optimization — 2026-11-10

The user accepted the phase-aligned LLPC/ACO performance baseline and saved
frame-6 first nonzero divergence case, and requested an archive and commit
before further optimization. This checkpoint includes the baseline integration,
capture tools, runtime diagnostic fixes, tests, reports and small textual
evidence. No new enc_stage_96_128_128_fl_lt optimization has begun.

## Accepted state

- Branch codex/mesa-aco; strict all-ACO D3D12, global/vector8, capture v4,
  pinned Mesa 26.2.4 commit 96cb43121031992b85767f9c1be8f3f48e22b1d2.
- Latest six-pair Windows D3D12/PAL baseline: LLPC/ACO steady medians
  3.14242/3.71610 ms (+18.26% ACO time); DRS 3.01784/3.54100 ms (+17.34%).
- Fast remains within existing quality gates and reproduces the saved
  frame-6 dispatch-3 attribute-9 case at pixel (203,378). Its specific
  floating evaluation-order explanation is recorded in the diagnostic report.
- Saved Legacy control is bit exact across 20 measured frames. No lowered
  precision, relaxed threshold or LLPC fallback is accepted.
- Reduced projection prefetch is rejected. Its isolated experiment scripts
  and evidence are retained as history, not applied to baseline shaders.

Read mesa-aco-runtime-diagnostic-results-2026-11-10.md,
mesa-aco-current-comparison-2026-11-10.md and
mesa-aco-prefetch-experiment-2026-11-10.md for findings and raw-evidence paths.
Reference implementation analysis and native Linux environment assessment are
in mesa-aco-runtime-diagnostic-preparation-2026-11-10.md.

## Local executable archive

Archive: build/aco-baseline-archive-2026-11-10/runtime-baseline.zip

Size: 8,314,758 bytes. SHA256:

    f3520dcdb7d24615b2e81b4c1eb81e1cbb436c6ba5bb20b3734e352763a0b0f9

The ZIP contains the exact current diagnostic ASI, benchmark, unit executable,
staged SPIR-V directory and baseline global/vector8 records/manifests. All
191 ZIP entries were hashed and verified against payload-manifest.json in
mesa-aco-baseline-checkpoint-2026-11-10/. It contains no model weights or
NVIDIA binaries. This local archive is under ignored build/ and is not in Git;
copy it separately when transferring the checkpoint to another machine.
The commit preserves its complete entry hashes and source/build instructions,
not the executable ZIP.

Large original readbacks remain in
build/aco-runtime-trace/runs-approved-2026-11-10/. The frame6-manifest.json
records hashes/sizes of all 142 files across the original LLPC/ACO trace-6
directories (69 binary readbacks plus metadata/log per compiler). Small CPU
replay inputs, compiler IR and comparison evidence are committed in
mesa-aco-runtime-diagnostic-2026-11-10/. Large raw readbacks are not added to
Git and must be preserved separately if the ignored build tree is cleaned.
No raw texture/weight dump is promoted into tracked documentation.

## Verification and next boundary

Before the commit: eight CPU address-capture tests and ten CPU trace-comparator
tests passed; the timing script parses; git diff --check passed. No GPU baseline
was repeated for this archival task. Previously documented builds and GPU
checks remain the validation evidence for this unchanged implementation.

Stop after this commit and report its identifier. Subsequent work, after the
checkpoint handoff, is to inspect actual spill, live ranges and address
calculation in enc_stage_96_128_128_fl_lt and propose one machine-code-backed
factor. Confirm expected generated-code change before the saved correctness
case/all-frame checks, then runtime timing. Do not repeat the complete accepted
baseline table or revive the rejected projection-prefetch candidate.
