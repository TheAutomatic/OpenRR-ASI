# OpenRR Stage 9 (Wave D) — 7 维理论上限终审与发行门禁签收（v016）

- 工作树/分支：`codex/mesa-263-comparison`（代码与构建终点 **`67d5ab2`**，前序 Stage 9 Round 3 检查点 `be5b1ce` / `4d7c0ae`，父封板 `b26472a` = tag `stage8-seal-b26472a`；未 `git push`）
- 最终全绿洁净发行包：`build/aco-v015/stage/`（冻结副本 `build/v015-runs/stage9/final-pkg/stage/`，双方严格 **`296/296` 文件 SHA-256 逐字节一致、`0` 个 `*.log` 残留**；所有终验跑分均在隔离副本 `build/v015-runs/stage9/eval-work/stage/` 与 `cold-install/` 完成）
- 过程证据：[`docs/test-evidence/v016-stage9-wave-d-progress.md`](v016-stage9-wave-d-progress.md)（第一轮～第四轮全过程、误判平反、负例隔离与终版重测）
- ISA 方法：全部采用 ACO 原生后端（`AMDGPU_GPU_ID=gfx1201 RADV_DEBUG=shaders`，截取 `After lowering to hw instructions:` `print_program`；裸 `llvm-mc-21` 因 GFX12 `v_cvt_pk_fp8_f32` 首字 `0xd769` 误码导致 4 字节错位，已全面废止）

> [!NOTE]
> **Stage 9 Round 4（`67d5ab2`）晋级与整改总揽**：
> 1. **此前晋级保留**：WP-D1a（`MRG8_W` UT8 尾段偏移修复 + `enc_stage_64_64_96`/`enc_stage_96_128_128` `LT_MRG8=1`，并补充 `enc_96` 干净隔离 ABBA 验证）、WP-D4（`_f_lt` native 白名单）、第三轮 ACO 解沙袋（6 处 `--cmat-address-shaders` 补齐 + Mesa 26.3 `ffma_weak@16 -> ffma` + `float_mul_add16` 融合 + 37 个零升档 `_lt`/`_lx` 核解锁 `precision=native`）及 D6.0 `XINF/XINQ`。
> 2. **本轮新晋级两项结构收益核**：**WP-D3 `W2_96UP=1`**（`dec_up_96_128_128` 切入 `onr_block_w2.glsl` 2-wave，probe-free `3×ABBA` **`23/24` 腿全胜**，`@idx 4K` 自身 `-43.7 µs`）与 **WP-D6 `LT_PV8=1`**（`dec_dfn_128_128` Normalized `P·V` 全 FP8 乘加、`192 VGPR` 零溢出，`4K 3×ABBA` `mean -9.3 µs / med -45.1 µs`、`@idx 4K` `_fl_lt -23.8 µs` / `_f_lt -4.2 µs`）。
> 3. **审计缺口全闭环**：`openrr_contrast_eval.cpp` 消除缺参考帧伪 `999 dB` 并为全部 `140` 帧回读独立扫描 `NaN/Inf`；新增 `Item NFMA` 抓出并修复 `downsample*_lt` 在 `native` 下折叠 `half_rz` 边界比较导致 `>= 65504` 溢成 `+Inf` 的缺陷（4 支恢复 `global`）；`build_aco_records.py` 补齐捕获失败原子撤回、`build_shaders.py` 补齐陈旧 `.spv` 清理与 `LT_PV8` GFX11 剥离、`THIRD_PARTY.md` 校准至 `Mesa 26.3.0` (`0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4`)；补齐 `W1A8`、`D2a loadstore/WGP`、`D2b WGP`、`enc_96 nomrg8` 与官方 `v0.1.6`（含 `Legacy=0`）全套无探针 `3×ABBA`。

## 维度 1 — 全网算子精度与静态/动态 ISA 终表（ACO 原生后端）

22 个在跑核（4K Quality `@idx` 窗口，`isa_dim1.json` + `_f_lt` 增补样本）静态指令账（`v_wmma_f32_16x16x16_fp8_fp8` / `v_wmma_f16_16x16x16_f16` / `v_cvt_pk_fp8_f32` / `v_maxmin3|v_minmax3` / `v_cndmask*` / `v_perm_b32` / `s_setreg*`；VGPR / LDS / scratch / exec）：

| 核 | fp8 | f16 | cvt_pk_fp8 | maxmin | cndmask | perm | setreg | VGPR | LDS | scratch | exec B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `pooled_history2_lt` | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 24 | 0 | 0 | 964 |
| `d3d_ae` | 0 | 0 | 0 | 3 | 6 | 0 | 0 | 24 | 1024 | 0 | 1740 |
| `d3d_enc0_attr_f_lt` | 0 | 0 | 0 | 57 | 237 | 46 | 0 | 96 | 0 | 0 | 22524 |
| `enc0_stage_32_64_f_lt` | 96 | 116 | 192 | 0 | 24 | 24 | 14 | 192 | 0 | 0 | 24356 |
| `enc_stage_64_64_96_f_lt` | 240 | 104 | 152 | 0 | 2 | 256 | 13 | 240 | 9728 | 0 | 25688 |
| `enc_stage_96_128_128_fl_lt` | 408 | 118 | 160 | 0 | 0 | 240 | 7 | 256 | 25600 | 0 | 37340 |
| `enc_stage_128_128_160_fl_lt` | 192 | 136 | 96 | 0 | 0 | 320 | 8 | 256 | 29696 | 0 | 29184 |
| `enc_stage_160_256_0_fl_lt` | 40 | 36 | 84 | 0 | 0 | 80 | 6 | 192 | 30208 | 0 | 14656 |
| `enc_stage_160_256_160_fl_lt` | 40 | 76 | 100 | 0 | 1 | 80 | 8 | 192 | 30208 | 0 | 19804 |
| `dec_up_160_256_160_fl_lt` | 100 | 26 | 124 | 0 | 0 | 80 | 24 | 192 | 30208 | 0 | 19272 |
| `dec_dfn_160_256_fl_lt` | 40 | 36 | 84 | 0 | 4 | 81 | 6 | 192 | 30208 | 0 | 15548 |
| `dec_up_128_128_160_fl_lt` | 170 | 128 | 104 | 0 | 0 | 140 | 24 | 240 | 21504 | 0 | 23736 |
| `dec_dfn_128_128_fl_lt` (**`LT_PV8`**) | **192** | **104** | **168** | 0 | 4 | 65 | 30 | **192** | 9728 | 0 | 22980 |
| `dec_up_96_128_128_fl_lt` (**`W2_96UP`**) | **192** | **56** | **160** | 0 | 0 | 87 | 24 | **256** | **12288** | 0 | 22048 |
| `dec_dfn_96_128_fl_lt` | 96 | 68 | 112 | 0 | 7 | 112 | 6 | 256 | 9728 | 0 | 17776 |
| `dec_up_64_64_96_f_lt` | 176 | 96 | 128 | 0 | 0 | 48 | 28 | 240 | 9728 | 0 | 21280 |
| `dec_dfn_64_64_f_lt` | 128 | 104 | 96 | 0 | 7 | 77 | 10 | 240 | 9728 | 0 | 18992 |
| `dec_up_32_64_64_f_lt` | 48 | 132 | 80 | 0 | 8 | 32 | 12 | 192 | 4096 | 0 | 16132 |
| `dec_head_32_64_f_lt` | 16 | 132 | 48 | 12 | 52 | 20 | 4 | 192 | 0 | 0 | 20492 |
| `post_f_lt` | 0 | 0 | 0 | 12 | 134 | 0 | 0 | 96 | 8320 | 0 | 15828 |
| `d3d_downsample_lt` (**`global`**) | 0 | 0 | 0 | 12 | **10** | 0 | 0 | 48 | 2784 | 0 | 2048 |
| `clear_range` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 24 | 0 | 0 | 200 |

*(注：`dec_dfn_128_128_f_lt` 非 FLOW 默认态为 `fp8=192, f16=104, cvt=168, cndmask=3, perm=65, setreg=28, VGPR=192, LDS=9728, code=21752`；`dec_up_96_128_128_f_lt` 为 `fp8=192, f16=56, cvt=160, perm=64, setreg=26, VGPR=256, LDS=12288, code=20500`。)*

- **精度策略 (`global` vs `native`) 边界测试（`Item NFMA`）**：
  - `ONR_ACO_PRECISION=native` 虽开启 `float_mul_add16 |= nir_float_muladd_support_fuse` 与 `ffma_weak@16 -> ffma`，但在 `d3d_downsample_lt` / `d3d_downsample_tl_lt` / `downsample_lt` / `downsample_tl_lt` 中把 `half_rz()`（`onr_common.glsl:38`）内的舍入后比较 `abs(float(h)) > abs(x)` 折叠抹除，使输入 `>= 65504` 时下溢出为 `+Inf`。本轮 `test_asi_units.cpp` 新增 `Item NFMA`（覆盖 FP16 奇数尾数中点、`±0`、次正规数、`±65504` 边界及 22 核端到端有限性）并将这 4 支 `downsample*_lt` 迁回 `global`，其余 **`37` 支 `_lt`/`_lx` 核保留 `native`**，`test_asi_units.exe` 验证 0 次位错与全部 `v_pk_fma_f16` 融合完好。
- **残留 `f16` WMMA 说明**：
  - `C=32` 的 `116–132` 条 = `LT_Q16`/`LT_VO` fp16 近似路径（`W1A8` 实测全替换为 FP8 在画质 `-0.291 dB / -0.327 dB` 与无探针 `3×ABBA` 性能 `+2.97% / +2.65%` 均双败）；
  - `C≥64`：经 `LT_PV8=1` 在 `dec_dfn_128_128` 实证推广——**Softmax 归一化后 `P ∈ [0, 1]` 乘以 256（`[0, 256] ⊂ [0, 448]`）的 `P·V` 可零额外全局访存转成 FP8**（静态 `f16` WMMA `-32`、`fp8` WMMA `+32`）；未动部分为未归一化的 `Q·K^T`（需逐 tile `2^k` `act-scale` 动态缩放）、与头维/通道重排紧耦合的 `O·W_out`（对 `240/256 VGPR` 核如 `W2 C=64`、`enc_96`/`enc_128`/`dec_dfn_96` 易触发驻留下降），暂保留精确 FP16 并留作下一代 `act-scale` 入口。

## 维度 2 — 全 6 档预设稳态 vs 官方 `OpenRR v0.1.6` & `RTX 5090 (310.9.1.0)` 终表

`final-presets` 在隔离运行包 `eval-work/stage/` 下于默认实机态（`ONR_RR_FLOW=0`，warmup 64 / 400 帧稳态，无探针）重测：

| 预设 | RTX 5090 参照 | Legacy=0 中位 | New=1 中位 (`vs 5090`) | Fast=2 中位 (`vs 5090`) | 官方 v0.1.6 New / Fast（独跑参照） |
|---|---|---|---|---|---|
| `1080p Quality` | `0.871 ms` | `3.6490 ms` | **`2.8034 ms`** (`3.22x`) | **`2.6379 ms`** (**`3.03x`**) | `2.9578` (`-5.22%`) / `2.8307` (`-6.81%`) |
| `1440p Quality` | `1.407 ms` | `6.2007 ms` | **`4.8675 ms`** (`3.46x`) | **`4.6384 ms`** (**`3.30x`**) | `5.1732` (`-5.91%`) / `5.0184` (`-7.57%`) |
| `4K Quality` | `2.915 ms` | `13.7152 ms` | **`10.6831 ms`** (`3.66x`) | **`10.2940 ms`** (**`3.53x`**) | `11.4246` (`-6.49%`) / `11.1023` (`-7.28%`) |
| `4K Performance` | `1.958 ms` | `8.7480 ms` | **`6.7376 ms`** (`3.44x`) | **`6.4507 ms`** (**`3.29x`**) | `7.0734` (`-4.75%`) / `6.8917` (`-6.40%`) |
| `1080p DLAA` | `1.545 ms` | `7.1631 ms` | **`5.6296 ms`** (`3.64x`) | **`5.3872 ms`** (**`3.49x`**) | `6.0198` (`-6.48%`) / `5.8531` (`-7.96%`) |
| `4K UltraPerf` | `1.287 ms` | `5.1932 ms` | **`3.9185 ms`** (`3.04x`) | **`3.7410 ms`** (**`2.91x`**) | `4.0323` (`-2.82%`) / `3.8944` (`-3.94%`) |

- **4K Qual `@idx` 口径（`FLOW=1`，600 帧，`final-unshackle-idx-*-4k` vs `pre-unshackle-9ebfb19`）**：
  - `New=1`：纯神经网络 16 核和 **`8,097.8 -> 8,035.3 µs`**，22 核总和 **`10,529.0 -> 10,469.3 µs`（`−59.7 µs`）**；主力变化：`dec_dfn_128_128_fl_lt −23.8 µs`（`LT_PV8`）、`dec_up_64 −14.2`、`dec_up_96 −11.2`、`enc0 −10.7`、`enc_64 −9.9`、`dec_up_32 −6.5`（注：`dec_dfn_96_fl +24.0 µs` 为 `FLOW=1` 持久核上游 `dec_up_96` 提前推进后的行标志等待转移）。
  - `Fast=2`：纯神经网络 16 核和 **`7,727.3 -> 7,639.6 µs`**，22 核总和 **`10,158.9 -> 10,077.6 µs`（`−81.3 µs`）**；主力变化：`dec_head_32_lx −26.7`、`dec_dfn_128_fl −22.3`、`dec_up_32_lx −13.9`、`dec_up_64 −12.4`、`enc0_lx −12.1`、`dec_up_96 −10.1 µs`。
- **解沙袋 + Wave D 全量累计 `3×ABBA`（`67d5ab2` vs 冻结 `pre-unshackle-9ebfb19`，默认 `FLOW=0`，24/24 腿全胜，`max_leg_dev <= 0.74%`）**：
  - `New=1 @ 1080p Qual`：`2.8174 -> 2.7953 ms`（**`−22.1 µs / −0.79%`，6/6 胜**，中位差 `−28.0 µs`）
  - `New=1 @ 4K Qual`：`10.7866 -> 10.6979 ms`（**`−88.6 µs / −0.82%`，6/6 胜**，中位差 `−81.7 µs`）
  - `Fast=2 @ 1080p Qual`：`2.6612 -> 2.6334 ms`（**`−27.8 µs / −1.04%`，6/6 胜**，中位差 `−23.5 µs`）
  - `Fast=2 @ 4K Qual`：`10.3934 -> 10.2568 ms`（**`−136.6 µs / −1.31%`，6/6 胜**，中位差 `−133.8 µs`）

### 2.1 对标官方上游原版 `OpenRR v0.1.6` 发行版（`OpenNrRrRuntime.dll`）同台实测

- **官方包校验**：`https://github.com/OpenNeuralRendering/OpenRR/releases/download/v0.1.6/openrr-0.1.6.zip`（`3,669,743` B，SHA256 `B27B40117792FC4B456D99F396F787EB6EE272D7F4890660FDB73BC64F2FA528`；内含 `OpenNrRrRuntime.dll` SHA256 `B8B4399A80B69A1D1AE6ABA3F15C449CADEFD76DE2AA4B79CCDB29160286829F` + 93 `.spv`）。上游开箱默认 `FLOW=1`，我方开箱默认 `FLOW=0`。
- **Anchor #1 位级校验（我方 `Legacy=0` `aco-strict` vs 官方 `OpenNrRrRuntime.dll` `llpc`）**：`final-anchor1-1080p` 与 `final-anchor1-4k` 在 `steady` 与 `drs` 各末帧（`coverage="phase_last_frame"`，**共 4 张输出表面**）及 `final-up016-abba-leg-{1080p,4k}` 全部 **12/12 条腿稳态末帧**，均维持 **`max_ulp_diff = 0`、`exact_percent = 100%`、`non_finite = 0`**。*(仅校验实际落盘对比的 4 张 `phase_last_frame` 与 12 条 ABBA 末帧，不外推为跨未测场景的"绝对 0-ULP"。)*
- **交错 `3×ABBA` 正面对决（`67d5ab2` `eval-work/stage` vs 官方 `OpenNrRrRuntime.dll`，240f，`--align-engine-phase 0`，10 组共 60 腿，单腿离散 `<= 1.53%`）**：

| 对比项 | 口径 | 官方 v0.1.6 均值 (中位) | 我方终版均值 (中位) | 帧时差 (`ours − up`) | 我方提速比 (`up / ours`) | 胜腿 |
|---|---|---|---|---|---|---|
| `Legacy=0` @ 1080p Qual | 开箱默认（`0-ULP`） | `3.3651 ms` (`3.3655`) | `3.6268 ms` (`3.6223`) | `+261.6 µs` (`+7.78%`) | `−7.21%` | 0/6 (`100% 0-ULP`) |
| `Legacy=0` @ 4K Qual | 开箱默认（`0-ULP`） | `12.8636 ms` (`12.8440`) | `13.5629 ms` (`13.5780`) | `+699.3 µs` (`+5.44%`) | `−5.16%` | 0/6 (`100% 0-ULP`) |
| **`New=1` @ 1080p Qual** | 开箱默认（上 F1 vs 我 F0） | `2.9579 ms` (`2.9528`) | **`2.7988 ms`** (`2.7996`) | **`−159.1 µs` (`−5.38%`)** | **`+5.68%`** | **6/6** |
| **`New=1` @ 4K Qual** | 开箱默认（上 F1 vs 我 F0） | `11.4606 ms` (`11.4344`) | **`10.6693 ms`** (`10.6260`) | **`−791.3 µs` (`−6.90%`)** | **`+7.42%`** | **6/6** |
| **`Fast=2` @ 1080p Qual** | 开箱默认（上 F1 vs 我 F0） | `2.8182 ms` (`2.8147`) | **`2.6345 ms`** (`2.6366`) | **`−183.7 µs` (`−6.52%`)** | **`+6.97%`** | **6/6** |
| **`Fast=2` @ 4K Qual** | 开箱默认（上 F1 vs 我 F0） | `11.1293 ms` (`11.1101`) | **`10.2498 ms`** (`10.2478`) | **`−879.4 µs` (`−7.90%`)** | **`+8.58%`** | **6/6** |
| **`New=1` @ 1080p Qual** | 同流控锁定（双 `FLOW=1`） | `2.9523 ms` (`2.9520`) | **`2.8127 ms`** (`2.8130`) | **`−139.6 µs` (`−4.73%`)** | **`+4.96%`** | **6/6** |
| **`New=1` @ 4K Qual** | 同流控锁定（双 `FLOW=1`） | `11.3208 ms` (`11.3259`) | **`10.5868 ms`** (`10.5754`) | **`−734.1 µs` (`−6.48%`)** | **`+6.93%`** | **6/6** |
| **`Fast=2` @ 1080p Qual** | 同流控锁定（双 `FLOW=1`） | `2.8121 ms` (`2.8102`) | **`2.6331 ms`** (`2.6317`) | **`−179.0 µs` (`−6.36%`)** | **`+6.80%`** | **6/6** |
| **`Fast=2` @ 4K Qual** | 同流控锁定（双 `FLOW=1`） | `11.0138 ms` (`11.0088`) | **`10.1551 ms`** (`10.1488`) | **`−858.7 µs` (`−7.80%`)** | **`+8.46%`** | **6/6** |

结论：`New=1` 与 `Fast=2` 对官方原版 `v0.1.6` **`48/48` 腿全胜**；开箱默认下 4K `Fast=2` 相对官方 `v0.1.6` 快 **`−0.879 ms`（`−7.90%` / `+8.58%` 提速）**，`New=1` 快 **`−0.791 ms`（`−6.90%` / `+7.42%` 提速）**。`Legacy=0` 因全局 IEEE `aco-strict` 保证 `0-ULP` 而慢 `5.4%–7.8%`；兼顾画质与速度的日常首选模式为 `New=1`（vs 5090 `3.66x @4K`、`52.08 dB+`），追求更高帧率选 `Fast=2`（`3.53x @4K Qual`、`2.91x @4K UltraPerf`）。

## 维度 3 — 同宿主同 SPIR-V ACO vs 驱动 LLPC（内部回退惩罚检验，非原版速度基线）

> [!IMPORTANT]
> **对照 ③ 口径重申**：同包同 SPIR-V 下切换 `--baseline-compiler llpc` 与 `--candidate-compiler aco-strict` 量化的是**"离线 ACO 与同套 FP8 着色器强走驱动 LLPC 即时编译之间的回退惩罚"**，绝不是对比官方原版 `v0.1.6`。

终版 `67d5ab2` `3×ABBA @ 4K Qual`（`FLOW=0`）：
- **`New=1`（`final-dim3-aco-vs-llpc-new-4k`）**：LLPC `13.4793 ms` vs ACO **`10.8168 ms`** -> ACO 快 **`−2,662.5 µs`（`−19.75%`，中位 `−2,499.8 µs`，6/6 胜**；其中 `g2c1` LLPC 腿高至 `14.4383 ms`，其余 5 腿均在 `−2,284..−2,754 µs`）。证明改造后的 `mode-4 saturatedConvertEXT` 着色器必须配发 `.nrp` 记录、不可裸退 LLPC。
- **`Legacy=0`（`final-dim3-aco-vs-llpc-leg-4k`）**：同包相同 SPIR-V + LLPC（`FLOW=0`）`13.0730 ms` vs ACO `aco-strict` `13.6956 ms`（`+622.6 µs / +4.76%`, 0/6 wins，`100% 0-ULP`），直观分离出全局 IEEE `exact` 与驱动默认优化对纯 `Legacy` 核的代价（与上表 `Legacy` 对官方 `v0.1.6` 的 `+5.44%` 自洽）。

## 维度 4 — Mochi 加法周期模型对账（4K Qual 实测更新）

`128 SIMD @ ~3.0 GHz = 384,000 SIMD-cycles/µs`（4K Quality `pad=2560×1440`）：
- `enc0_stage_32_64_f_lt`（`58,101` waves）：`96×8 + 116×16 = 2,624` WMMA cycles/wave -> 纯 WMMA `~397.0 µs` vs 终版 `final-unshackle-idx-new-4k` **`1,308.5 µs`**（占比 `~30.3%`）；
- `enc_stage_64_64_96_f_lt`（`29,302` waves）：`240×8 + 104×16 = 3,584` cycles/wave -> 纯 WMMA `~273.5 µs` vs 终版 **`855.6 µs`**（占比 `32.0%`）；
- `dec_dfn_128_128_fl_lt`（**`LT_PV8=1`**：`192` fp8 + `104` f16，`192 VGPR`）：`146.8 -> 123.0 µs`（`−23.8 µs / −16.2%`；`FLOW=0` `179.4 -> 175.2 µs`）。

## 维度 5 — 核间屏障气泡与 `FLOW` 终裁

| 口径 | 22 核和 µs | 核间空隙和 µs | 稳态中位 |
|---|---|---|---|
| Round 2 STAMPS (`p1`) `FLOW=1` / `FLOW=0` | `10,536.8` / `10,561.7` | `175.0` / `66.4` | `10.6674` / `10.6120 ms` |
| **终版 STAMPS (`final-flow01-idx-4k`) `FLOW=1` / `FLOW=0`** | `10,442.9` / `10,431.2` | `539.6` / `480.5` | `10.7072` / **`10.6483 ms`** |
| **终版无探针 `3×ABBA` (`final-flowab-4k`) `FLOW=1` vs `FLOW=0`** | — | — | `10.6836` vs **`10.6548 ms`** (`F1−F0 = +28.8 µs`, med **`+33.0 µs`**) |
| **终版无探针 `3×ABBA` (`final-flowab-1080p`) `FLOW=1` vs `FLOW=0`** | — | — | `2.8251` vs **`2.8131 ms`** (`F1−F0 = +12.0 µs`, med **`+12.5 µs`**) |

**终裁**：在无探针严格 `ABBA` 交错（`final-flowab-{4k,1080p}`）下，默认值 **`FLOW=0` 在 4K（`−28.8 µs`）与 1080p（`−12.0 µs`）均不慢于甚至略快于 `FLOW=1`**；与正式包跨栈 `v0.1.6`（`FLOW=1` 对齐仅在 4K 因持久热态略差 `-82.5 µs`）综合保持接近，且 **Anchor #2 全部 `140/140` 帧经 `frame_compare.csv` 实测 `FLOW=0` 与 `FLOW=1` `max_ulp = 0`、`total_diff_halfs = 0`（逐位完全一致）**。宿主默认维持安全的 **`FLOW=0`**（`runtime.cpp:924`）。

## 维度 6 — 各通道宽度波数/驻留终表

| C | 核 | 线程 | 体 | VGPR | waves/SIMD | LDS | 变化说明 |
|---|---|---|---|---|---|---|---|
| 32 | `enc0`/`dec_up_32`/`dec_head_32` | 32 | W1 | 192 | 8 | 0–4 KB | 保留 `LT_Q16`/`LT_VO`（`W1A8` `216/240 VGPR` + 画质/性能双败归档） |
| 64 | `enc_64`/`dec_up_64`/`dec_dfn_64` | 64 | W2 | 240 | 6 | 9.7 KB | `enc_64` `LT_MRG8=1` |
| **96** | `enc_96`/`dec_dfn_96` | 64–128 | W2/TR | 256 | 6 | 9.7–25.6 KB | `enc_96` `LT_MRG8=1`（`nomrg8` `+3.8/+6.8 µs` 隔离证实保留） |
| **96 (up)** | **`dec_up_96_128_128`** | **64** | **W2 (`W2_96UP=1`)** | **256** | **6** | **12.3 KB** | **Stage 9 Round 4 晋级**：4 套无探针 `3×ABBA` **`23/24` 腿胜** |
| **128** | `enc_128`/`dec_up_128`/`dec_dfn_128` | 128–256 | TR | 192–256 | 6–8 | 9.7–29.7 KB | `dec_dfn_128` **`LT_PV8=1`（`192 VGPR`, `9.7 KB`）晋级**；`C=128 W2` 实测溢出/截断已关闭 |
| 160 | `enc_160×2`/`dec_up_160`/`dec_dfn_160` | 256 | TR | 192 | 8 | 30.2 KB | `shared-cmat=load/loadstore` 与 WGP 均无收益保留原状 |

## 维度 7 — 图像壳物理带宽 Roofline 与非矩阵核终账（4K Qual）

理论最小流量 `~1.225 GB/帧` / `644 GB/s` ≈ `1.90 ms` 下限。终版 `final-unshackle-idx-new-4k` 图像壳与辅助核实测：`post_f_lt = 1,217.9 µs`、`d3d_enc0_attr_f_lt = 855.0 µs`、`d3d_downsample_lt = 266.5 µs`（恢复 `global` 后与 `9ebfb19` `264.3 µs` 基本持平）、`d3d_ae = 56.2 µs`、`pooled_history2_lt = 38.1 µs`、`clear_range = 0.3 µs`；主三核合计 `2,339.4 µs`（约 `2.34 ms`，有效带宽利用率 `~81%`）。

---

## WP-D1～D6 逐项最终裁决矩阵

| 项 | 最终裁决 | 关键实测依据 |
|---|---|---|
| **D1a** (`enc_64`/`enc_96` `LT_MRG8` + UT8 尾段修复) | **晋级保留** | 修复 `p1-abba-4k` 符号误记：实为 **`−55.4 µs`（`6/6` 胜，最好 `−104.3 µs`）**、`1080p −6.5 µs`（`5/6`）。clean 隔离实验 `d1a-enc96-nomrg8` 拿掉 `enc_96 LT_MRG8` 后：`@idx +17.5 µs`、`4K 3×ABBA +3.8 µs`（仅 2/6 胜）、`1080p +6.8 µs`（仅 1/6 胜），独立验证 `enc_96` 单开为正 |
| **D1b** (`enc_160 MRG8_UNROLL`) | **归档回退** | `216 VGPR`、`40,448 B LDS`（均破门槛），保持禁用 |
| **D2a** (`dec_dfn_160` WGP / `shared-cmat`) | **归档回退** | 撤回 `shared-cmat=load` 伪向量化表述（实为同 SHA256 A/A）；补测 **`loadstore`**（新哈希 `e79bbfb8318163c7`/`1c8b3af526051861`）：`@idx −0.2 µs`、`4K 3×ABBA +13.5 µs`（`1/6`）；DFN WGP 无探针 `4K 3×ABBA −1.2 µs`（`3/6` 噪声内） |
| **D2b** (`post_f_lt` WGP / `LT_POST_STILL`) | **归档回退** | Clean 重跑 `post` WGP `4K 3×ABBA +20.2 µs`（`2/6`）、`1080p` 中位 `−2.5 µs`（`4/6` 但带 `+783.6 µs` 毛刺）；`LT_POST_STILL`（`onr_post_main.glsl:130-139` `subgroupAll(ix == 0 && iy == 0)`）按波次判定精确纹素中心，合成运动集 `+10.3 µs` |
| **D3** (`W2_96UP=1` / `C=128 W2`) | **`W2_96UP=1` 晋级**；`C=128 W2` 关闭 | **平反晋级**：前轮单次有探针负判被 4 套无探针 `3×ABBA` 推翻（**`23/24` 腿胜**：4K New `−73.5 µs`, 1080p New `−11.7 µs`, 4K Fast `−36.7 µs`, 1080p Fast `−9.3 µs`），`@idx 4K`（`FLOW=0`）`459.9 -> 416.2 µs`（`−43.7 µs`）且下游 `−1.6 µs`、`output_sha256` `140/140` 逐字节一致。`C=128 W2` 实测 `dec_up_128` 触发 `onr_block_w2.glsl:96` `#error`、`dec_dfn_128` `NT <= 6` 截断通道导致 `10.28 dB` 退化而关闭 |
| **D4** (`_f_lt` native + `FLOW=0/1`) | **已合入** | 4 个 `_f_lt` native + 4 个 `downsample*_lt` `global` 安全回退（`Item NFMA`）；无探针 `FLOW=0/1` `4K +28.8 µs / 1080p +12.0 µs`（`F0` 不慢于 `F1`）+ `140/140` `0-ULP`，维持 `FLOW=0` |
| **D5** (`LT_W1A8`) | **归档回退** | 修复 5 Bug 后 `216/216/240 VGPR`（超 `<= 192` 目标，`scratch=0`）；units `k_lite` 系 `New=1 (_lt)`；Anchor #2 `−0.2910 dB`（vs seal）/ `−0.3270 dB`（vs `d1a`）；双分辨率无探针 `3×ABBA` 双输 `0/6`（4K **`+316.8 µs / +2.97%`**、1080p **`+73.9 µs / +2.65%`**） |
| **D6.0 & D6** (`XIN` 拆平 & `LT_PV8`) | **`LT_PV8=1` (`dec_dfn_128`) 晋级** | 撤回 `W1A8` 跨通道外推与 `<180 µs` 未测净上限；`XINF/XINQ` 中性保留；实作 **`LT_PV8=1`**（`onr_block_fast.glsl:403-411, 979-1015, 1068-1099`）：`192 VGPR`、FP8 WMMA `+32` / FP16 `−32`、Anchor #2 最差仅 `−0.0195 dB`、`@idx 4K` `_fl_lt −23.8 µs` / `_f_lt −4.2 µs`、`4K 3×ABBA −9.3 µs (med −45.1 µs, 4/6 wins)`、`1080p −0.9 µs`；其余 `C≥64` `Q·K^T` 因未归一化 logits 留待 `act-scale` |

## 第 8 项 — 最终发行门禁全量签收单（基于洁净终包 `67d5ab2`）

| 门禁 | 实测结果 |
|---|---|
| **包洁净度与 `296/296`  parity** | ✅ `build/aco-v015/stage/` 与 `final-pkg/stage/` 各 **`296` 文件、100% SHA-256 一致、`0` 个 `*.log` 残留**（Post-Gate 复检通过）；`THIRD_PARTY.md` 标明 `Mesa 26.3.0 (0a6fa60a)` |
| **94/94 `.nrp` 与原子构建防呆** | ✅ `manifest.json` `records=94`、`failures=[]`、磁盘实存 `94/94` `.nrp`（`0` 孤儿）、全核 `scratch_bytes == 0`；失败路径原子写 `failures.json` 并撤回清单；**`50/50` Legacy `.nrp` 较 `pre-unshackle-9ebfb19` SHA-256 逐字节不变** |
| **Anchor #1 (`Legacy=0` vs 官方 `v0.1.6`)** | ✅ `final-anchor1-1080p` & `final-anchor1-4k` 覆盖 `steady`+`drs` **4 张 `phase_last_frame` 输出**，以及 `final-up016-abba-leg-{1080p,4k}` **`12/12` 条 ABBA 腿末帧**，全部 **`max_ulp_diff = 0`、`100% exact`、`non_finite = 0`** |
| **Anchor #2 三列 (`New=1` / `Fast=2` / `FLOW`)** | ✅ 全回读帧 **`420/420` `total_non_finite = 0`**（`140` 帧/次）；缺参考 `79+14=93` 帧显式记 `missing_ref` / `null`（2 曝光探针 ABI 跳过）。实命中 5090 参考 **`v1 18/97` + `v2 29/43`**：<br>• `New=1`：`Q_S0_4k_perf = 52.0832`, `Q_S0_1080p_dlaa = 52.4210`, `Q_S1_1080p_qual = 39.1149`, `Q_S1_4k_qual = 47.7555`, `Q_S2_1080p_qual = 40.6842`, `Q_S2_4k_perf = 41.2340`, `P00 = 49.1560 dB`（vs `4332470` worst `−0.0371 dB`、vs 合流 `m2` worst `−0.0109 dB`、vs `Stage8-Seal` worst `−0.0264 dB`）<br>• `Fast=2`：`51.4667` (`+0.0423` vs `s8-exp`), `51.7635` (`+0.0438`), `38.9516` (`−0.0160`), `47.3550` (`−0.0501`), `40.2771` (`−0.0525`), `40.8747` (`+0.0133`), `P00 = 47.6201 dB`（`d3d_downsample_lt` 回归 `global` 修复 `>= 65504` `+Inf`，同时拉升 `Q_S2_1080p_qual f15/31/63` `+0.97..+1.10 dB`）<br>• **`FLOW=0` vs `FLOW=1`**：`frame_compare.csv` **`140/140` 帧 `diff_halfs = 0, max_ulp = 0`** |
| **`shaders-gfx11` 94 核** | ✅ 含 `LT_PV8` 在内全量 FP8 宏剥离：`capability 4212/4213 = 0`、`Float8 = 0` |
| **4 套单测与策略套件** | ✅ `test_aco_build_policy.py`（**11/11 OK**）、`test_present_scope.exe` PASS、`test_dxgi_queue.exe` PASS、`test_asi_units.exe`（含 **`[PASS] Item NFMA`** + `ALL ASI UNIT TESTS PASSED`） |
| **冷装验收 + 语言字体 + PE 表** | ✅ `cold-install` `fm=0/1/2` 共 `310` 次 `[openrr compiler]` ACO 命中（`50/130/130`）、覆盖 `94/94` SPV、`LLPC=0`；四语包 + `NotoSansCJKsc-Regular.ttf`（`21,659,488 B`）；`openrr-amd.asi`（`3,396,738 B`）导出 `1` 组 `InitializeASI`、导入 `7` 洁净系统 DLL |
| **仓库与行尾纪律** | ✅ 单分支 `codex/mesa-263-comparison`，全改动 **100% LF（`CR=0`）**，`git diff --check` 干净，工作树干净，严格未 push |
