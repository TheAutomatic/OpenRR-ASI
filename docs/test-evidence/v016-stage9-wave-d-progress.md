# OpenRR Stage 9 (Wave D) 执行进展（进行中）

工作树 `codex/mesa-263-comparison`（父锚点 `b26472a`，本地 tag `stage8-seal-b26472a`；冻结对照包 `build/v015-runs/stage9/frozen-stage8-seal/`，58 MB）。本文随轮次更新；终审见 Phase 3 的 `v016-stage9-wave-d-final-audit.md`。

## 0. 冻结与基线

- tag `stage8-seal-b26472a` 已打；`frozen-stage8-seal/` 含 `stage/` + `openrr-trace.asi`。
- 画质对照锚：`build/v015-runs/stage8/final-remed-anchor2/`（= 封包包的 v1 76 / v2 44 结果）。

## 1. WP-D1 — MRG8_W 偏移修复 + enc_64 / enc_96 重开 LT_MRG8（晋级）

1. **缺陷修复**（`gpu/shaders/include/onr_block_w2.glsl`）：`MRG8_W` 原无条件按 U8 尾段 `2*(Co*4C + C*4*Cin)` 取址；`ENC_CIN == 0` 的 enc2+（UT8 打包）真实尾段在 `16*Co*C`。该 bug 此前已被 `enc_stage_128_128_160`（已开 LT_MRG8、CIN=0）实际踩中——本轮修复同时纠正了它。fast 体（`onr_block_fast.glsl` MRG_W8）原本就正确。
2. **重开**（`gpu/fastmode.cmake`）：`LT_DEFS_enc_stage_64_64_96` / `LT_DEFS_enc_stage_96_128_128` 各加 `LT_MRG8=1`；gfx11 剥离表（CMakeLists / build_shaders.py）已含 LT_MRG8，零 FP8 影响。
3. **资源档位**（manifest，vs seal）：enc_64_f_lt VGPR 240→240、LDS 不变；enc_96_f_lt 256→256；enc_96_fl_lt 240→**256**、LDS +6 KB（FLOW 镜像）；均 `scratch=0`。
4. **单测**：`test_asi_units` 新增 WP-D1 断言——enc1 U8 尾段 `2*(Co*4C+C*4*Cin)` 与 enc2..enc4 UT8 尾段 `16*Co*C` 精确命中 `mrg_w` e4m3 码、fp16 前缀逐字节一致（D3D12 打包为地址子片，经 `fixed_` blob 解析回读；`-0.0` 的符号位差异按数值零等价处理）。全 PASS。
5. **Anchor #2**（frozen `d1a-mrg8-pkg`，fast-aco，vs seal `final-remed-anchor2`）：v1 全用例 `+0.0000 .. +0.0377 dB`；v2_motion 最差 `−0.0980 dB`（Q_S2_4k_perf 41.1592），未触 −0.25 dB 单步警戒、远高于固定账本 −0.70 dB 硬线；non_finite=0。
6. **快速计时**（test_asi_units 引擎档，24 帧短测）：4K Fast p50 **10.944 ms**（seal 档 11.096，−152 µs），1080p 2.852（seal 2.847，噪声带内）。@idx 4K 配对与 3×ABBA 待终包跑。


### 1.1 @idx 4K 配对（p1-candidate vs frozen-stage8-seal，600 帧，FLOW=1，drift 带内）

| 核 | seal µs | cand µs | Δ |
|---|---|---|---|
| enc_stage_64_64_96_f_lt（MRG8 开，U8 尾段） | 898.1 | 860.2 | **−37.9（−4.2%）** |
| enc_stage_96_128_128_fl_lt（MRG8 开，UT8 修复尾段） | 520.3 | 519.3 | −1.0 |
| 22 核总和 | 10575.0 | 10541.9 | −33.1 |
| 稳态中位（summary.json） | 10.6464 ms | 10.5898 ms | **−56.6 µs（−0.53%）** |

未改核 ±0.0…+2.7 µs（≤0.36%，带内）。勘误：seal 中 `enc_stage_128_128_160` 虽带 `LT_MRG8=1`，但 C=128 走 TR 体（`onr_block_fast.glsl` MRG_W8 本就正确），W2 体的偏移 bug 从未实例化——属潜伏缺陷，本轮由 enc_96（C=96 W2 体）首次启用时修复并验证。

### 1.2 3×ABBA（p1-candidate vs frozen-stage8-seal，FLOW=1，锁步 .asi+data-dir）

| 档 | 中位差 | 均值差 | 最差腿 | 噪声带（±0.23/0.26%） |
|---|---|---|---|---|
| 4K Qual | −4.9 µs | +2.9 µs | +104.3 µs（单腿毛刺） | ±24 µs |
| 1080p Qual | +2.9 µs | +0.7 µs | 10.0 µs | ±7.4 µs |


整帧 ABBA 在噪声带内（核级 −33 µs ≈ 0.3%，处于 ABBA 分辨率边缘）；核级配对（drift 校正）与稳态中位（−56.6 µs）一致向好，画质全绿 → **D1a（enc_64/enc_96 MRG8 + 偏移修复）晋级**。

## 2. WP-D1b — enc_160 MRG8_UNROLL（归档回退）

`MRG8_UNROLL`（`onr_block_fast.glsl` q 循环 `[[dont_unroll]]`）+ enc_160_256_160 `LT_MRG8=1` 实测：VGPR 192→**216**（目标 ≤192 未守住）、LDS 30208→**40448**（+10 KB）。按计划归档回退：宏保留（ inactive），fastmode 已还原。代码字节 −492 但驻留双破门槛。

## 3. WP-D4 — `_f_lt` native 白名单补齐（合入）

`asi/build/build_asi.sh` `--precision-native-only` 补入 `dec_up_128_128_160_f_lt / dec_dfn_160_256_f_lt / enc_stage_160_256_0_f_lt / enc_stage_160_256_160_f_lt` 四个实机默认 `ONR_RR_FLOW=0` 变体。manifest 验证 4 核 `precision=native`，VGPR/LDS/scratch 不变（仅 code 字节变化）。

### 3.1 FLOW=0 vs FLOW=1 正面对决（p1-candidate，4K Qual，600 帧，STAMPS）

| 模式 | 稳态中位 | p95 | 22 核和 | 核间空隙和 |
|---|---|---|---|---|
| `ONR_RR_FLOW=1`（跑分口径） | 10.6674 ms | 11.1366 | 10536.8 µs | 175.0 µs |
| `ONR_RR_FLOW=0`（D3D12 宿主默认） | **10.6120 ms** | **10.9453** | 10561.7 µs | **66.4 µs** |

FLOW=0 净赢 ~55 µs（空隙 −108.6 µs 吸收 barriers 串行 +24.9 µs；32→34 持久核尾波饥饿气泡消除，与计划预判一致）。终审按裁决边界双列两套权威水位（默认实机态 FLOW=0 / 流控态 FLOW=1），不混标；不改宿主默认（`runtime.cpp:924` 维持 FLOW=0）。

## 4. WP-D5 — LT_W1A8 原生 e4m3 全 FP8 头（实现完毕，性能止损归档）
实现（保留于源码，未启用）：`onr_w1_head.glsl` 新增 `LT_W1A8` 全 FP8+FP32 头（Q/V/S/O/W_out 全 `v_wmma_f32_16x16x16_fp8_fp8`，寄存器转置 `W1A_TA`，f32 归一 `W1A_PB`，`id8` 精确 e4m3 单位阵，`W1_WOUT8` = `UT8(b.out)` 尾段 `4*C*X`，`W1_QA8/W1_VB8` = `qv_bytes` 码区 `2*X*C`）；`onr_block_w1.glsl` 提供 xq 一次量化 + 跨头 `oc` QC8 累加器 + `LT_MLP8` 依赖门；engine.cpp `UT8(b.out, ...)` 尾段与 `e4m3_rne` 导出 + 16 级 stage 偏移/数值单测（全 PASS）。

五项证据（vs seal，per kernel enc0 / dec_up / dec_head）：

| 项 | enc0 | dec_up | dec_head |
|---|---|---|---|
| fp8 WMMA | 96→**200** | 48→**222** | 16→**258** |
| f16 WMMA | 116→**0** | 132→**0** | 132→**8** |
| VGPR | 192→216 | 192→216 | 192→216 |
| v_cndmask | 477→592 | 193→**735** | 179→**851** |
| code B | 24392→30588 | 16156→24148 | 20876→32160 |

- 反汇编（llvm-mc-21）：`v_cvt_pk_fp8_f32` 与 `v_maxmin` 均为 **0** —— 扫描全包 94 核（含 seal 基线同核）证实：**本 26.3 ACO 工具链下 `qe4_pair32` 的 f2e4m3fn_satfn 硬件折叠在全包任何位置都不发生**，软件量化（cndmask+perm+s_setreg）是现状路径；W1A8 每头 ~40 个量化 tile 的软件代价超过 WMMA 8→16 周期收益，叠加 VGPR 216（驻留 4→3 waves/SIMD）导致净回退。
- 快速计时（units 引擎档）：4K 11.086 ms（比 D1a +142 µs / +1.3%，超 ±0.5% 漂移带），1080p 2.900。
- 裁决：按止损条款归档——代码保留（`LT_W1A8` 未定义即零影响），fastmode 还原为 `LT_Q16/LT_VO`。重启前置条件：工具链侧 `v_cvt_pk_fp8_f32` 折叠恢复（或找到可匹配形态）后，量化成本每 tile ~30 ops → 1 op，账本将反转为净赢。

## 5. 其他已落地（待终包测量）

- **WP-D2a**：`build_aco_records.py` 新增 `--shared-cmat-shaders`（逐核限定 `ONR_ACO_SHARED_CMAT` 作用域；指纹含 `shared_cmat_shaders`；记录含 per-record `shared_cmat`）；`test_aco_build_policy.py` 扩到 8 测全绿（作用域隔离、缓存指纹、off+名单报错、全局模式兼容）。
- **WP-D3 前置**：`onr_block_w2.glsl` LT_UP8 路径支持 `ENC_CP == 128`（`w3` 记录 + `UP_K3_8`），`>128` 拒绝；`W2_96UP` 门已存在，启用留待测量。
- **WP-D6 前置**：`XIN2Y`（w2 体）拆为 fma 段 + 集中量化段（压 `s_setreg` MODE 翻转）；`XQ8Y` 审视为纯量化无需拆。

## 6. 构建/门禁记录

- 全部构建：WSL `build_asi.sh build/aco-v015` + `build_runtime_trace.sh`；94/94 记录、0 failures、`scratch=0` 每轮核验。
- 套件：`test_aco_build_policy`（8/8）、`test_present_scope`、`test_dxgi_queue`、`test_asi_units`（含新 WP-D1/D5 断言 + 引擎 1080p/4K 档）全部 PASS。
- 事故 1：anchor2 首跑指向正被 W1A8 重建覆写的 `build/aco-v015` → "missing metadata" 假失败；改为冻结包 `-Pkg` 后通过（教训：测量一律用冻结目录）。
