# Optional-guide compiler comparison — 2026-11-11

Baseline: Mesa branch 65a64cf, upstream v0.1.4. RX 9070 XT, driver 26.10.1.
Same runtime objects, identical SPIR-V per comparison, LLPC versus strict ACO.
1280x720 -> 1920x1080; 8 warmup, 8 steady, 12 DRS; phase aligned; every measured frame checked.
Synthetic fixture reuses hit/color/motion textures as optional guides; these findings are not a captured game workload.

## Findings

- Legacy with all optional guides: fails strict bit-exact gate, but steady last frame differs in only 8 half-floats by 1 ULP.
- Fast: all nine single-guide cases pass existing quality thresholds.
- DOF + responsivity: PASS. Transparency layer + opacity + motion: PASS.
- Disocclusion + SSS + color-before-transparency + specular motion: PASS.
- Disocclusion + SSS + responsivity + DOF: FAIL (steady p99 absolute error 0.00695801, gate 0.006). This is a reproducing subset, not a proven minimal combination.
- All six non-transparency guides: FAIL, steady PSNR_01=43.8267 dB.
- All guides with LT_VO removed: still FAIL on steady frames 2..4, but last-frame PSNR_01=63.1065 dB, matching old-version observations. With LT_VO enabled the previous last-frame result was 47.06 dB.

## Interpretation and limits

Evidence supports cross-compiler numerical sensitivity under combined inputs, amplified by LT_VO in this fixture. It does not prove that Fast is designed to be correct only on LLPC, nor identify an incorrect instruction.
Wide guides alone pass. Independent wide allocation is not established as the cause. Disabling LT_VO does not fully fix the issue; no production switch or threshold was changed.
No intermediate-dispatch first-divergence trace was captured in this run. Next: minimize the four-input subset, compare reset/first-frame versus accumulated history, then capture matched inputs/intermediates for both compilers.
Before tracing wide guides, fix the diagnostic output selection: current aco_runtime_trace.hpp snapshots e.attr_ even when enc0_attr writes e.attr_wide_. Otherwise the trace can compare the wrong buffer.

## Reproduction

Use build/asi-014/guide_bench.exe with the same arguments as the prior parity runs and --optional-guides 1.
OPENRR_BENCH_GUIDE accepts comma-separated field names; unset means all original optional guides.
Names: transparency_layer, transparency_opacity, transparency_motion, disocclusion_mask, sss_guide, responsivity_mask, color_before_transparency, depth_of_field_guide, specular_motion.
Production data: build/asi-014/stage/openrr-amd; no-VO experiment: build/asi-014/no-vo.
No-VO was built from a copied gpu source tree with LT_VO=1 removed from fastmode.cmake and matching ACO records regenerated. Runtime weights retain both original and preprojected values, selected by the shader macro.
Individual summaries are archived beside this document. Raw outputs/logs remain under build/asi-014.
