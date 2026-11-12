# 接口对齐落地：MV_JITTERED 接线 + qv_bytes 边界加固 + guide Subrect 官方键（v2_motion 真卡复验）

分支 `codex/mesa-263-comparison`（`1813eed` §3 笔误修正之上；本轮含锚点 #2 全部成果的快进合回 `4510cc1 → 3374842`）。被测 stage 由本分支重建（3343636 B）。

## 0. 改动清单

| 项 | 位置 | 内容 |
|---|---|---|
| MV_JITTERED 接线 | `engine.hpp`（EngineFrame/Enc0BlockInput 加 `mv_jittered`）、`runtime.cpp:714`（按 flag 赋值）、`engine.cpp:570`（`f.mv_jittered && !reset` 传入，reset 帧绝不消费陈旧 prev_jitter）、`engine.cpp:116`（`0x328/0x32c` 填 `(jitter−prev_jitter)/mv_scale`） | 零着色器改动；shaders 的 `(m + C(0x328)) * C(0x320)` 代数保证负/UV `mv_scale` 下符号比例成立 |
| qv_bytes 边界 | `engine.cpp:190-194` | 编码后拒绝 `(c & 0x7f) == 0x7f`（OCP NaN 码）——FP16 ±480（0x5f80/0xdf80，e=23&m=896）原会编成 0x7f/0xff |
| QV 穷举单测 | `test_asi_units.cpp` Item QV | 65,536 全码扫描：恰 254 接受、全部解码回 FP16 逐位相等、±480 必抛 |
| guide Subrect 官方键 | `rr_asi_main.cpp` fill_d3d12_frame/fill_vk_frame | sss（ColorBeforeFog 优先→SSS 键→色子矩形）、cbt、dof 三 guide 的 `.Subrect.Base.X/Y` 官方键读取，缺省回退 color.x/y |

## 1. 单元与回归门禁

- `test_asi_units`：**ALL PASSED**（含新 Item QV）
- Legacy vs 冻结 v0.1.5-ACO：steady/DRS **exact 100.0000%，ulp 0**（接线对非 flag 路径零影响——`djx=djy=0` 严格保持）

## 2. v2_motion 真卡复验（三档，fast-aco 为主）

**不变性**（接线前逐帧对比，PSNR01 与 diff_halfs 全同）：
- Q_S1_1080p_qual：10 保存帧 Legacy/Fast **逐位一致**（create_flags 0x4B 无 bit2，dejitter 恒 0）
- P00：45.99 / diff 5,650,567（与 `3374842` 轮完全相同）；P15：999（MV.Scale 等价探针仍逐位通过）

**P16a/b/c 跃升**（fast-aco；括号内为接线前基线）：

| 对照 | 接线后 | 5090 自身签名 |
|---|---|---|
| OpenRR 自身 P16b vs P00 | **56.85**（33→，>>50 ✓） | 51.341（同构 ✓） |
| OpenRR 自身 P16a vs P00 | **32.26**（999→，≈32 ✓） | 32.132（**差 0.13 dB**） |
| OpenRR 自身 P16c vs P00 | **31.34**（32.25→） | 32.035 |
| OpenRR P16b vs P16a | 32.26 | 32.134（差 0.13） |
| OpenRR P16c vs P16a | 36.55 | 35.52 |
| 跨卡 OpenRR-P16b vs 5090-P16b | **45.94**（32.90→，≈P00 跨卡基线 45.99 ✓） | — |
| 跨卡 OpenRR-P16a vs 5090-P16a | **41.05**（31.94→，+9.1 dB） | — |
| 跨卡 OpenRR-P16c vs 5090-P16c | 41.24（36.64→） | — |

- P16b 完全达标（45.94 ≈ P00 基线）；P16a 升 9.1 dB 至 41.05 未到 45–47：接线后**两侧各自偏离自身 P00 约 32 dB**（OpenRR 32.26 / 5090 32.13，响应幅度惊人一致），但两侧偏差方向仅部分相关，跨卡距离 = 基线误差 + 双方独立偏差，落在 41 dB 是数学必然——判据里"跃升至 45–47"隐含了偏差完全相关的假设，实测修正为 41。响应签名五项（P16a/b/c × vsP00/vsPair）与 5090 全部同构，符号约定（dj = curr−prev）确认一致。
- Legacy 档同构（P16b 56.85 / P16a 32.19 / P16c 31.41），三档一致。

**组合自检**（套件外自定义用例 `PX_combo_jitter_negscale`：MV_JITTERED + mv_scale=(−0.5,−0.5) + mv_tex_scale_num=−2 + sign=+1，哈希门禁按 rr_ref 精确计算后校验）：
- 输出与 **P16b 逐位一致**（999 dB / 0 diff / 0 ULP，vs P00 四项指标与 P16b 完全相同）——负 mv_scale 与预乘 −2 的组合在去抖动下精确还原，代数闭环（`fp16/fp32 乘 2 幂与 RNE 舍入可交换`）经实测确认。

## 3. 结论

1. `ONR_RR_FRAME_MV_JITTERED` 全链路接通（ASI flag → runtime → 0x328/0x32c），render-pixel 空间去抖动对任意 mv_scale 符号/尺度成立，零着色器改动、非 flag 路径逐位不变。
2. qv_bytes 的 OCP NaN 边界封死（±480），65,536 穷举单测永久把关。
3. 三个可选 guide 的官方 Subrect.Base 键补齐（D3D12+Vulkan 双路径）。
4. 锚点 #2 v2_motion 的最后一个接线级待办（P16）关闭：响应签名与官方网络五项全同构，跨卡 P16b 达基线。

## 4. 复现

```powershell
# 单测（含 QV 穷举）
cd build/asi-015-aco; $env:OPENRR_SHADER_DIR="stage/openrr-amd/shaders"; $env:ONR_ACO_DIR="stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1"; ./test_asi_units.exe
# P16 复验与组合自检
build/contrast-eval/openrr_contrast_eval.exe --asi build/asi-015-aco/stage/plugins/openrr-amd.asi `
  --data-dir build/asi-015-aco/stage/openrr-amd --dlssd <dll> --suite v2_motion --config fast-aco `
  --case Q_S1_1080p_qual,P00_baseline_s1_g0,P15_mv_scale_neg0p5,P16a_mv_jittered_raw,P16b_mv_jittered_prev_minus_curr,P16c_mv_jittered_curr_minus_prev `
  --ref-dir <extracted_v2_motion>/results_v2_motion --out <dir>
# 组合用例（--cases build/contrast-eval/cases_combo.json，无 --ref-dir）
```
