# Current LLPC versus ACO runtime baseline — 2026-11-10

Current diagnostic build: LLPC steady median 3.14242 ms, strict ACO 3.71610 ms,
an ACO time increase of 18.26%. DRS medians are 3.01784 and 3.54100 ms, an
increase of 17.34%. Diagnostic fixes have not removed the performance gap.
This updates the previous approximate 19% result using a fresh paired test.

Correctness diagnostic repair established the saved Fast first divergence; it
did not make Fast bit exact or change the baseline shader arithmetic. See
mesa-aco-runtime-diagnostic-results-2026-11-10.md. The rejected projection
prefetch variant was not used in this comparison.

## Configuration and execution

Same build/aco-runtime-trace/openrr-trace.asi and openrr_bench.exe on both sides;
same build/asi/stage/openrr-amd shaders. Explicit LLPC versus strict all ACO.
ACO uses existing capture v4 global/vector8 gfx1201 records, Mesa 26.2.4 pinned
commit 96cb43121031992b85767f9c1be8f3f48e22b1d2. No new precision policy.

Actual Windows D3D12/PAL runtime, 1280x720 to 1920x1080, Fast, flow=1,
direct guides=1, direct output=1, sampler=1, optional guides disabled. Same
UploadPattern, projection, jitter and phase-aligned frame/reset sequence. The
user confirmed the GPU idle and authorized performance tests without further
permission. No Linux GPU performance claim is made.

First run a separate correctness control: 8 warmup, 8 steady, 12 DRS with all
20 measured outputs saved. Then six timing pairs, each with 24 warmup,
48 steady and 24 DRS frames. Three LLPC-first and three ACO-first pairs
alternate order. Timing runs disable trace and per-frame output readback;
phase-final readback is outside GPU timestamp intervals. Timestamped work
includes the runtime's complete recorded D3D12 reconstruction path.

Actual compiler and direct/sampler/flow logs are checked for both labels in
all six runs. After asynchronous building calls are excluded, the per-frame
engine phase, render dimensions, reset and trace fields match between sides.

## Timing results

Each row contains the run's frame-time median in milliseconds.

| Round | First | LLPC steady | ACO steady | LLPC DRS | ACO DRS |
|---|---|---:|---:|---:|---:|
| 0 | LLPC | 3.14680 | 3.70628 | 3.05252 | 3.54692 |
| 1 | ACO | 3.14504 | 3.72028 | 2.99844 | 3.53508 |
| 2 | LLPC | 3.13012 | 3.72256 | 3.01032 | 3.48652 |
| 3 | ACO | 3.14432 | 3.70424 | 3.07180 | 3.52628 |
| 4 | LLPC | 3.13408 | 3.71496 | 2.96916 | 3.55780 |
| 5 | ACO | 3.14052 | 3.71724 | 3.02536 | 3.58016 |

| Statistic | LLPC | ACO | ACO time increase |
|---|---:|---:|---:|
| Median of six steady medians | 3.14242 | 3.71610 | 18.26% |
| Steady per-run median range | 3.13012–3.14680 | 3.70424–3.72256 | paired 17.78–18.93% |
| Median of six DRS medians | 3.01784 | 3.54100 | 17.34% |
| DRS per-run median range | 2.96916–3.07180 | 3.48652–3.58016 | paired 14.80–19.83% |

The ratio uses the medians across runs, not a single fastest run. DRS has
more variation than steady state. The stable steady gap reproduces the earlier
finding; the change from approximately 19% to 18.26% does not establish a
performance improvement from diagnostic fixes. No operator change is included,
and prior and current batches are separate measurement windows.

## Correctness and interpretation

The fresh control passed all 20 original Fast per-frame quality gates, maximum
44 ULP, minimum PSNR_01 61.4079 dB, no non-finite outputs. All 40 output files
(20 per compiler) match the saved fast-aligned outputs by SHA256. Existing
nonzero errors remain reproducible; there is no new measured numerical change.
All six timing runs also passed their phase-final output gates; those checks
are not presented as all-frame correctness coverage. Legacy was not rerun
because this batch tests unchanged Fast shaders. Its saved 20-frame exact
control remains the Legacy baseline.

These results measure the current integration and compiler configuration.
They do not identify a PAL field or a particular kernel as the cause. The
first reduced-projection-prefetch experiment preserved correctness but did not
improve performance; it remains rejected. Further optimization should target
an observed kernel-level instruction/resource issue and then confirm runtime
benefit against this baseline.

## Reproduce and evidence

    powershell -File asi/tests/run_runtime_trace_batch.ps1 -Stage Control -TraceName fast-current-llpc-aco-control -Output build/aco-runtime-trace/runs-approved-2026-11-10
    powershell -File asi/tests/run_llpc_aco_current_timing.ps1
    python3 asi/tests/summarize_llpc_aco_current.py

The summary command is CPU-only and runs in WSL. Small summaries, order files,
frame-state CSVs and correctness hashes are archived beside this report in
mesa-aco-current-comparison-2026-11-10/. Large control readbacks and raw logs
remain in build/aco-runtime-trace/runs-approved-2026-11-10/, under
fast-current-llpc-aco-control and llpc-aco-current-timing.

Worktree codex/mesa-aco remains uncommitted. No released ASI, staged shader,
baseline record or original package was overwritten.
