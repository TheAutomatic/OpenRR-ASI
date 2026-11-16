# OpenRR Stage 9 (Wave D) — 7 维理论上限终审与发行门禁签收（v016 Round 5 闭环）

- 工作树 / 分支：`codex/mesa-263-comparison`（Round 5 代码终点 **`f6dcc83`**；前序 Round 4 `2bbe662` / `67d5ab2`、Round 3 `4d7c0ae` / `be5b1ce`、父封板 `b26472a` = tag `stage8-seal-b26472a`；未执行 `git push`）
- 最终全绿洁净发行包：`build/aco-v015/stage/`（冻结副本 `build/v015-runs/stage9/final-pkg/stage/`，双方严格 **`296/296` 文件 SHA-256 逐字节一致、`0` 个 `*.log` 残留**；全部实机门禁与性能跑分均在隔离副本 `build/v015-runs/stage9/eval-work/` 与 `r5-final-cold-install/` 完成）
- 过程证据：[`docs/test-evidence/v016-stage9-wave-d-progress.md`](v016-stage9-wave-d-progress.md)（Round 1～5 全过程、误判平反、`A1`/`A2` 仲裁、`[P1]`/`[P2]` 闭环）
- ISA 方法：全部采用 ACO 原生后端（`AMDGPU_GPU_ID=gfx1201 RADV_DEBUG=shaders`，截取 `After lowering to hw instructions:`；裸 `llvm-mc-21` 因 GFX12 `v_cvt_pk_fp8_f32` 首字 `0xd769` 误码导致 4 字节错位，已全面废止）

> [!NOTE]
> **Stage 9 Round 5（`f6dcc83`）终局晋级与审查闭环总揽**：
> 1. **核心晋级配置锁定（`gpu/fastmode.cmake`）**：
>    - 保留前序晋级：WP-D1a（`MRG8_W` UT8 尾段修复 + `enc_64`/`enc_96` `LT_MRG8=1`）、WP-D4（37 核 `_lt`/`_lx` `native` + `4` 支 `downsample*_lt` 安全 `global` 回退）、6 处 `--cmat-address-shaders`、Mesa 26.3 `ffma_weak@16 -> ffma` 与 `float_mul_add16` 融合、D6.0 `XINF/XINQ`、**WP-D3 `W2_96UP=1`**（仅限 `gfx12` `(64,1,1)`，`gfx11` 已显式剥离并保持 `(128,1,1)`）。
>    - **WP-D6 Round 5 仲裁与多宽度推广（`A2` 降级 + `D6a C=64×3` 晋级）**：400 帧无探针单变量 `3×ABBA` 证实 `dec_dfn_128_128 LT_PV8=1` 在 `1080p F0`（`+0.17 µs, 3/6`）与 `4K F1`（`+6.63 µs, 2/6`）落入噪声/微负带，默认配置已关回 OFF（着色器保留可按需开启）；同期扩展 `W2` 与 `TR SPLIT=2` 对剩余 `C>=64` 宽度逐组原型实测，**晋级 `D6a pv8-c64`（`enc_stage_64_64_96`、`dec_up_64_64_96`、`dec_dfn_64_64` 三核 `LT_PV8=1`，均保持 `240 VGPR / 9,728 B LDS / 0 scratch`）**——400 帧无探针 `3×ABBA` **`4K −49.37 µs (−0.469%, 6/6 胜)`、`1080p −15.49 µs (−0.553%, 6/6 胜)`**，Anchor #2 最差仅 `−0.0427 dB`（vs `nopv8`）/ `−0.0899 dB`（vs `4332470`）；`D6b C=96 / C=128-upenc / C=160` 全数实测归档。
> 2. **Round 4 `[P1]` 与 `[P2]` 审计发现全闭环**：
>    - **`[P1]` `Fast=2` (`exp-aco`) 归因破案**：`Stage 8` 与 `Round 3` 测试使用 `build_runtime_trace.sh:9` 所编 `openrr-trace.asi`，该构建脚本漏传 `-DONR_LT_DEEP="\"2,4,2,4\""`（等效运行时 `ONR_RR_DEEP=0`、`VRAM 209 MB`），而正式交付 `openrr-amd.asi`（`build_asi.sh:39` / `gpu/CMakeLists.txt:242`）默认编译 `ONR_LT_DEEP="2,4,2,4"`（第 4 帧起启用 level-4 时域复用、`211 MB`）。撤销 Round 4 文档对 `d3d_downsample_lt` `global` 的不准确归因；经 4 组同二进制矩阵复测，`ONR_RR_DEEP=0` 精确复现 `P00=48.5126 dB` / `Q_S1 f63=48.5272 dB`（vs Round 3 `48.5386 / 48.4364`），且终版 **`exp-aco` 与 `fast-aco` 均实证 `FLOW=0/1` `140/140` 帧 `0-ULP` 完全一致**。
>    - **`[P2]` 工程与单测闭环**：`build_aco_records.py` 重构为 `.openrr-d3d12-v1.staging.<pid>` 目录内全量写入校验后再 `os.replace` 原子切换（`test_aco_build_policy.py` **`13/13 OK`**，含 `Ctrl+C` 中断保留旧包与 `gfx11` `(128,1,1)` 校验）；`openrr_contrast_eval.cpp` 加入 `--dump-frames`/`--compare-frames` 同路径互斥护栏、`summary.json` `"env"` 字段与全回读帧 `total_non_finite`；`test_asi_units.cpp` 校准 `65504.0`（精确最大正规半精度）vs `65600.0`（超界溢成 `+Inf` 后由 `half_rz` 钳至 `0x7BFF`），扩展覆及 4 支 `downsample*_lt`、`pooled_history2_lt`、`k_lite/k_lx` 全 22 核及 `OnrRrApi4::Record` Direct D3D12 guides（`NaN/±Inf/-0/subnorm/±65504` 全 `0 non-finite`）。

## 维度 1 — 全网算子精度与静态/动态 ISA 终表（ACO 原生后端）

默认 **`ONR_RR_FLOW=0`** 22 个在跑核（`r5-isa-dim1.json`，`RADV_DEBUG=shaders` + `aco_capture`；统计 `v_wmma_f32_16x16x16_fp8_fp8` / `v_wmma_f16_16x16x16_f16` / `v_cvt_pk_fp8_f32` / `v_maxmin*|v_minmax*` / `v_cndmask*|v_dual_cndmask*` / `v_perm_b32` / `s_setreg*`；VGPR / LDS / scratch / exec）：

- **22 核静态 WMMA 汇总**：
  - `New=1` 默认 `F0`：**`fp8 = 2,022`, `f16 = 1,354`**（较 Round 4 `F0` `1,418` `f16`：`pv8-c64` 三核各 `−32` 共 `−96`，`dec_dfn_128` 退出 `+32`，净降 `64` 条静态 `f16` WMMA）；`F1`：**`fp8 = 2,238`, `f16 = 1,404`**。
  - `Fast=2`（`dec_head_32_64_f_lx` `f16=108, pk_fma_f16=276`）：`F0` **`fp8 = 2,022`, `f16 = 1,330`**；`F1` **`fp8 = 2,238`, `f16 = 1,380`**。

| 核（默认 `F0` 在跑 stem） | fp8 | f16 | cvt_pk_fp8 | maxmin | cndmask | perm | setreg | VGPR | LDS | scratch | exec B |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `pooled_history2_lt` | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 24 | 0 | 0 | 964 |
| `d3d_ae` | 0 | 0 | 0 | 3 | 6 | 0 | 0 | 24 | 1024 | 0 | 1740 |
| `d3d_enc0_attr_f_lt` | 0 | 0 | 0 | 59 | 234 | 46 | 0 | 96 | 0 | 0 | 22512 |
| `enc0_stage_32_64_f_lt` | 96 | 116 | 192 | 0 | 23 | 24 | 14 | 192 | 0 | 0 | 24356 |
| **`enc_stage_64_64_96_f_lt` (`LT_PV8`)** | **272** | **72** | **248** | 0 | 2 | 256 | 27 | **240** | **9728** | 0 | 26808 |
| `enc_stage_96_128_128_f_lt` | 192 | 68 | 136 | 0 | 2 | 303 | 7 | 256 | 12288 | 0 | 23100 |
| `enc_stage_128_128_160_f_lt` | 192 | 136 | 96 | 0 | 0 | 320 | 8 | 256 | 29696 | 0 | 27856 |
| `enc_stage_160_256_0_f_lt` | 40 | 36 | 84 | 0 | 0 | 80 | 6 | 192 | 30208 | 0 | 13396 |
| `enc_stage_160_256_160_f_lt` | 40 | 76 | 100 | 0 | 1 | 80 | 8 | 192 | 30208 | 0 | 18380 |
| `dec_up_160_256_160_f_lt` | 100 | 26 | 124 | 0 | 0 | 80 | 24 | 192 | 30208 | 0 | 17992 |
| `dec_dfn_160_256_f_lt` | 40 | 36 | 84 | 0 | 3 | 81 | 6 | 192 | 30208 | 0 | 14284 |
| `dec_up_128_128_160_f_lt` | 170 | 128 | 104 | 0 | 0 | 140 | 24 | 240 | 21504 | 0 | 22476 |
| `dec_dfn_128_128_f_lt` (`LT_PV8=0`) | 160 | 136 | 72 | 0 | 3 | 65 | 20 | 192 | 9728 | 0 | 21036 |
| **`dec_up_96_128_128_f_lt` (`W2_96UP`)** | **192** | **56** | **160** | 0 | 0 | 64 | 26 | **256** | **12288** | 0 | 20500 |
| `dec_dfn_96_128_f_lt` | 96 | 68 | 112 | 0 | 5 | 112 | 6 | 256 | 9728 | 0 | 16732 |
| **`dec_up_64_64_96_f_lt` (`LT_PV8`)** | **208** | **64** | **224** | 0 | 0 | 48 | 38 | **240** | **9728** | 0 | 22404 |
| **`dec_dfn_64_64_f_lt` (`LT_PV8`)** | **160** | **72** | **192** | 0 | 5 | 77 | 24 | **240** | **9728** | 0 | 20020 |
| `dec_up_32_64_64_f_lt` | 48 | 132 | 80 | 0 | 8 | 32 | 12 | 192 | 4096 | 0 | 16132 |
| `dec_head_32_64_f_lt` | 16 | 132 | 48 | 12 | 36 | 20 | 4 | 192 | 0 | 0 | 20492 |
| `post_f_lt` | 0 | 0 | 0 | 28 | 134 | 0 | 0 | 96 | 8320 | 0 | 15936 |
| `d3d_downsample_lt` (**`global`**) | 0 | 0 | 0 | 20 | **10** | 0 | 0 | 48 | 2784 | 0 | 2048 |
| `clear_range`（`F1` 调度） | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 24 | 0 | 0 | 200 |

*(注：`FLOW=1` 对应的 `10` 个 `_fl_lt` 核：[`enc_stage_96_128_128_fl_lt`: `fp8=408, f16=118, cvt=160, VGPR=256, LDS=25600, exec=37340`]、[`enc_128_fl`: `29184 B`]、[`enc_160_0_fl`: `14656 B`]、[`enc_160_160_fl`: `19804 B`]、[`dec_up_160_fl`: `19272 B`]、[`dec_dfn_160_fl`: `cndmask=4, exec=15548 B`]、[`dec_up_128_fl`: `23736 B`]、[`dec_dfn_128_fl`: `cndmask=4, exec=22408 B`]、[`dec_up_96_fl`: `perm=87, setreg=24, exec=22048 B`]、[`dec_dfn_96_fl`: `cndmask=7, exec=17776 B`]。`d3d_downsample_lt` 额外统计：`v_pk_fma_f16=8`, `v_sub_u16=3`。`_lx` 系列 `dec_head_32_64_f_lx`: `f16=108, pk_fma_f16=276, exec=18128`。)*

- **精度策略 (`global` vs `native`) 边界测试（`Item NFMA` + `Item NFMA Direct D3D12 guides`）**：
  - **`65504.0`（`0x7BFF`）是半精度可精确表示的最大有限正规数**，高于该上界且超过半精度舍入中点的输入（`>= 65520.0`，测试使用 `65600.0`）在舍入为 `float16_t` 时首先成为 `+Inf`（`0x7C00`），再由 `onr_common.glsl:39` `half_rz()` 的 `abs(float(h)) > abs(x)` 判定向零回退为 `0x7BFF`。`ONR_ACO_PRECISION=native` 折叠抹除了该比较导致 `+Inf` 穿透；将 `4` 支 `downsample*_lt` 迁回 `global` 后全数通过，其余 `37` 支 `_lt`/`_lx` 保留 `native` 融合。Round 5 在 `test_asi_units.cpp:1272-1960` 补齐 `manifest.json` 核对（`4 global + 37 native`）、全部 4 支 `downsample{,_tl}_lt` / `d3d_downsample{,_tl}_lt`（含 D3D12 UAV slot 16）、`pooled_history2_lt`、`k_lite`/`k_lx` 全 22 核，以及直接喂入 `qNaN / ±Inf / -0.0 / subnormals / ±65504` 的 Direct D3D12 `OnrRrApi4::Record` 全链路验证（`0 non-finite`）。
- **残留 `f16` WMMA 与 `act-scale` 边界说明**：
  - `C=32` 保留 `LT_Q16`/`LT_VO`：实测完整 `W1A8` 更慢（`216/240 VGPR`、`384–496` 条 `v_cvt_pk_fp8_f32`、补回完整投影与独立 `W_out`，`4K +3.0%` 与 `−0.291 dB` 双输），而现行 `LT_VO` 在实数代数上融合了 `V·W_out`（落盘权重在 `engine.cpp:250` 舍入为 FP16）。
  - `C≥64`：经本轮扩展实验后，Softmax 后 `P·256` 定标 FP8（`LT_PV8`）已落在 **`C=64` 三核晋级**；未归一化的 `Q·K^T` 与紧耦合通道重排的 `O·W_out` 若采用 **`act-scale` 动态/离线缩放**（注：**`act-scale` 无需重新训练模型**——既可按 tile 在线求最大尺度，也可用现有本地样本离线标定后再做独立场景验证），仍需先评估其归约/打包代价及对 `240/256 VGPR` 核驻留的影响。

## 维度 2 — 全 6 档预设稳态（`F0`/`F1`）vs 官方 `OpenRR v0.1.6` & `RTX 5090 (310.9.1.0)` 终表

`r5-preset-*` 在隔离运行包 `eval-work/stage/` 下于 **`ONR_RR_FLOW=0`（默认态）** 与 **`ONR_RR_FLOW=1`（流控对齐态）** 实测（warmup 64 / 400 帧稳态中位，无探针）：

| 预设 | RTX 5090 | 官方 `v0.1.6` (`F1` New/Fast) | `F0` Leg / **New (`vs 5090`, `vs up`)** / **Fast (`vs 5090`, `vs up`)** | `F1` Leg / New (`vs 5090`, `vs up`) / Fast (`vs 5090`, `vs up`) |
|---|---|---|---|---|
| `1080p Quality` | `0.871 ms` | `2.9578` / `2.8307` | `3.6280` / **`2.8033 ms`** (`3.22x`, `−5.22%`) / **`2.6424 ms`** (**`3.03x`**, `−6.65%`) | `3.5845` / `2.8073` (`3.22x`, `−5.09%`) / `2.6321` (`3.02x`, `−7.02%`) |
| `1440p Quality` | `1.407 ms` | `5.1732` / `5.0184` | `6.1826` / **`4.7956 ms`** (`3.41x`, `−7.30%`) / **`4.5951 ms`** (**`3.27x`**, `−8.44%`) | `6.1451` / `4.8097` (`3.42x`, `−7.03%`) / `4.6031` (`3.27x`, `−8.28%`) |
| `4K Quality` | `2.915 ms` | `11.4246` / `11.1023` | `13.4709` / **`10.4662 ms`** (`3.59x`, `−8.39%`) / **`10.0888 ms`** (**`3.46x`**, `−9.13%`) | `13.3956` / `10.5164` (`3.61x`, `−7.95%`) / `10.1316` (`3.48x`, `−8.74%`) |
| `4K Performance` | `1.958 ms` | `7.0734` / `6.8917` | `8.6287` / **`6.6553 ms`** (`3.40x`, `−5.91%`) / **`6.3843 ms`** (**`3.26x`**, `−7.36%`) | `8.5427` / `6.6232` (`3.38x`, `−6.37%`) / `6.3797` (`3.26x`, `−7.43%`) |
| `1080p DLAA` | `1.545 ms` | `6.0198` / `5.8531` | `7.1248` / **`5.5664 ms`** (`3.60x`, `−7.53%`) / **`5.2787 ms`** (**`3.42x`**, `−9.81%`) | `7.0110` / `5.5266` (`3.58x`, `−8.19%`) / `5.2922` (`3.43x`, `−9.58%`) |
| `4K UltraPerf` | `1.287 ms` | `4.0323` / `3.8944` | `5.1219` / **`3.8810 ms`** (`3.02x`, `−3.75%`) / **`3.7215 ms`** (**`2.89x`**, `−4.44%`) | `5.0830` / `3.9067` (`3.04x`, `−3.12%`) / `3.7141` (`2.89x`, `−4.63%`) |

- **4K Qual `New=1` `@idx` 累计拆分（`r5-final-unshackle-idx-new-4k-{f0,f1}` vs `pre-unshackle-9ebfb19` 隔离副本，600 帧）**：
  - **默认 `FLOW=0`**：16 个 NN 核总和 **`8,119.5 -> 7,979.9 µs`（`−139.6 µs`）**，21 核总和 **`10,562.3 -> 10,423.2 µs`（`−139.1 µs`）**，整帧中位 **`10.6148 -> 10.4715 ms`**；主力变化：`dec_up_96_128_128 −46.0 µs`（`W2_96UP`）、`dec_up_64 −29.9`、`dec_dfn_64 −20.8`、`enc_64 −20.7`（`pv8-c64` + ACO unshackle）、`enc0 −8.3`、`dec_up_32 −5.0`、`dec_head_32 −3.8`、`dec_dfn_96 −2.7 µs`。
  - **`FLOW=1`**：16 个 NN 核总和 **`8,077.3 -> 7,978.5 µs`（`−98.8 µs`）**，22 核总和 **`10,522.0 -> 10,423.4 µs`（`−98.6 µs`）**，整帧中位 **`10.5786 -> 10.4758 ms`**（主要变化：`dec_up_64 −29.7`、`enc_64 −21.9`、`dec_dfn_64 −21.2`、`dec_dfn_128_fl −20.6`、`dec_up_96_fl −10.0`，`dec_dfn_96_fl +21.3` 为持久行标志等待转移）。*(注：`pre-unshackle-9ebfb19` 内随附的 `openrr-trace.asi` 在编译期未启用 `ONR_LT_DEEP="2,4,2,4"`，故 `Fast=2` 仅看无探针 `ABBA` 整帧，不做位置对齐 `@idx` 逐核拆分。)*
- **解沙袋 + Wave D 全量累计 `3×ABBA`（`f6dcc83` vs 隔离 `pre-unshackle-9ebfb19`，默认 `FLOW=0`，`24/24` 腿全胜，`max_dev <= 0.692%`）**：
  - `New=1 @ 1080p Qual`：`2.8146 -> 2.7804 ms`（**`−34.20 µs / −1.215%`，med `−31.18 µs`，6/6 胜**）
  - `New=1 @ 4K Qual`：`10.6056 -> 10.4659 ms`（**`−139.68 µs / −1.317%`，med `−140.95 µs`，6/6 胜**）
  - `Fast=2 @ 1080p Qual`：`2.6558 -> 2.6169 ms`（**`−38.91 µs / −1.465%`，med `−39.44 µs`，6/6 胜**）
  - `Fast=2 @ 4K Qual`：`10.2591 -> 10.0915 ms`（**`−167.65 µs / −1.634%`，med `−171.25 µs`，6/6 胜**）

### 2.1 对标官方上游原版 `OpenRR v0.1.6` 发行版（`OpenNrRrRuntime.dll`）交错 `3×ABBA`

- **官方包**：`openrr-0.1.6.zip`（`3,669,743` B，SHA256 `B27B40117792FC4B456D99F396F787EB6EE272D7F4890660FDB73BC64F2FA528`；内含 `OpenNrRrRuntime.dll` SHA256 `B8B4399A80B69A1D1AE6ABA3F15C449CADEFD76DE2AA4B79CCDB29160286829F` + 93 `.spv`）。上游默认 `FLOW=1`，我方默认 `FLOW=0`。
- **Anchor #1 位级校验（我方 `Legacy=0` vs 官方 `OpenNrRrRuntime.dll`）**：`r5-final-anchor1-{1080p,4k}` 在 `steady` 与 `drs` 各末帧（`coverage="phase_last_frame"`，**共 4 张输出**）及 `r5-final-up016-abba-leg-{1080p,4k}` 全部 **`12/12` 条 ABBA 腿末帧**，均维持 **`max_ulp_diff = 0`、`exact_percent = 100%`、`non_finite = 0`**。
- **交错 `3×ABBA` 正面对决（`f6dcc83` `eval-work/stage` vs 官方 `OpenNrRrRuntime.dll`，240f，`--align-engine-phase 0`，10 组 60 腿，单腿最大离散 `<= 0.438%`）**：

| 对比项 | 口径 | 官方 v0.1.6 均值 | 我方终版均值 | 帧时差均值 (`med_us`) | 我方提速比 (`up/ours - 1`) | 胜腿 |
|---|---|---|---|---|---|---|
| `Legacy=0` @ 1080p Qual | 开箱默认（`0-ULP`） | `3.3757 ms` | `3.6052 ms` | `+229.51 µs` (`+234.68`, `+6.799%`) | `−6.37%` | 0/6 (`100% 0-ULP`) |
| `Legacy=0` @ 4K Qual | 开箱默认（`0-ULP`） | `12.7627 ms` | `13.4331 ms` | `+670.40 µs` (`+669.45`, `+5.253%`) | `−4.99%` | 0/6 (`100% 0-ULP`) |
| **`New=1` @ 1080p Qual** | 开箱默认（上 F1 vs 我 F0） | `2.9532 ms` | **`2.7826 ms`** | **`−170.55 µs` (`−170.72`, `−5.775%`)** | **`+6.13%`** | **6/6** |
| **`New=1` @ 4K Qual** | 开箱默认（上 F1 vs 我 F0） | `11.2609 ms` | **`10.4678 ms`** | **`−793.08 µs` (`−793.65`, `−7.043%`)** | **`+7.58%`** | **6/6** |
| **`Fast=2` @ 1080p Qual** | 开箱默认（上 F1 vs 我 F0） | `2.8235 ms` | **`2.6177 ms`** | **`−205.75 µs` (`−204.38`, `−7.287%`)** | **`+7.86%`** | **6/6** |
| **`Fast=2` @ 4K Qual** | 开箱默认（上 F1 vs 我 F0） | `10.9738 ms` | **`10.0872 ms`** | **`−886.60 µs` (`−887.95`, `−8.079%`)** | **`+8.79%`** | **6/6** |
| **`New=1` @ 1080p Qual** | 同流控锁定（双 `FLOW=1`） | `2.9564 ms` | **`2.7987 ms`** | **`−157.70 µs` (`−159.58`, `−5.334%`)** | **`+5.63%`** | **6/6** |
| **`New=1` @ 4K Qual** | 同流控锁定（双 `FLOW=1`） | `11.2543 ms` | **`10.4632 ms`** | **`−791.07 µs` (`−790.05`, `−7.029%`)** | **`+7.56%`** | **6/6** |
| **`Fast=2` @ 1080p Qual** | 同流控锁定（双 `FLOW=1`） | `2.8245 ms` | **`2.6201 ms`** | **`−204.39 µs` (`−203.88`, `−7.236%`)** | **`+7.80%`** | **6/6** |
| **`Fast=2` @ 4K Qual** | 同流控锁定（双 `FLOW=1`） | `10.9902 ms` | **`10.0954 ms`** | **`−894.85 µs` (`−895.20`, `−8.142%`)** | **`+8.86%`** | **6/6** |

结论：`New=1` 与 `Fast=2` 对官方原版 `v0.1.6` **`48/48` 腿全胜**；开箱默认下 4K `Fast=2` 快 **`−886.60 µs`（`+8.79%` 提速）**，`New=1` 快 **`−793.08 µs`（`+7.58%` 提速）**；同流控 `FLOW=1` 下 `4K Fast=2` 快 **`−894.85 µs`（`+8.86%` 提速）**，`New=1` 快 **`−791.07 µs`（`+7.56%` 提速）**。兼顾画质与速度的首选为 `New=1`（`3.59x @4K Qual` vs RTX 5090、`52.08 dB+`），追求更高帧率选 `Fast=2`（`3.46x @4K Qual`、`2.89x @4K UltraPerf`）。

## 维度 3 — 同宿主同 SPIR-V ACO vs 驱动 LLPC（内部回退惩罚，4K + 1080p + 逐核 `@idx`）

> [!IMPORTANT]
> **对照 ③ 口径**：同包同 SPIR-V 下对比 `--baseline-compiler llpc` 与 `--candidate-compiler aco-strict` 量化的是**"离线 ACO 与同套 FP8 `saturatedConvertEXT` 着色器回退驱动 LLPC 之间的编译器成本"**（切换了整套编译器），不是跨栈对比官方 `v0.1.6`；单项优化隔离成本见上文及进展文档 `A1`/`A2`/`D3` 单变量对照。

终版 `f6dcc83` `3×ABBA` + 逐核 `@idx`（`ONR_RR_FLOW=0`）：
- **`New=1 @ 4K Qual`**（`r5-final-dim3-aco-vs-llpc-new-4k`, `max_dev=0.169%`）：LLPC `13.3580 ms` vs ACO **`10.4678 ms`** -> **ACO 快 `−2,890.22 µs`（`−21.637%`, med `−2,888.75 µs`, `6/6` 胜）**。同包 `@idx 4K`（`r5-final-dim3-idx-new-4k`）：16 个 NN 核 **`10,991.2 -> 7,981.5 µs`（`−3,009.7 µs`）**（`enc0 −638.6`, `dec_up_64 −419.7`, `enc_64 −412.8`, `dec_dfn_64 −364.9`, `dec_up_32 −321.4`, `dec_head_32 −251.8`, `dec_up_96 −172.1`），21 核合计 **`−2,858.0 µs`**；反向逆差主要在非矩阵图像壳 **`post_f_lt` `1,097.8 -> 1,226.1 µs`（`+128.3 µs`）** 与 **`d3d_enc0_attr_f_lt` `839.8 -> 858.4 µs`（`+18.6 µs`）**。
- **`New=1 @ 1080p Qual`**（`r5-final-dim3-aco-vs-llpc-new-1080p`, `max_dev=0.702%`）：LLPC `3.5040 ms` vs ACO **`2.7874 ms`** -> **ACO 快 `−716.57 µs`（`−20.450%`, med `−716.48 µs`, `6/6` 胜）**。同包 `@idx 1080p`（`r5-final-dim3-idx-new-1080p`）：16 个 NN 核 **`2,928.2 -> 2,178.7 µs`（`−749.5 µs`）**，21 核合计 **`−699.8 µs`**，其中 `post_f_lt` `275.2 -> 299.7 µs`（`+24.5 µs`）。
- **`Legacy=0 @ 4K Qual` & `@ 1080p Qual`（`100% 0-ULP`）**：4K LLPC `12.8488` vs ACO `13.4323 ms`（`+583.48 µs / +4.541%, 0/6`）；1080p LLPC `3.3830` vs ACO `3.6051 ms`（`+222.15 µs / +6.567%, 0/6`）。

## 维度 4 — Mochi 加法周期模型对账（4K Qual 与范围边界）

`128 SIMD @ ~3.0 GHz = 384,000 SIMD-cycles/µs`（4K Quality `pad=2560×1440`）：
- `enc0_stage_32_64_f_lt`（`58,101` waves）：`96×8 + 116×16 = 2,624` WMMA cycles/wave -> 纯 WMMA `~397.0 µs` vs 终版 `r5-final-unshackle-idx-new-4k-f0` **`1,319.3 µs`**（占比 `~30.1%`）；
- `enc_stage_64_64_96_f_lt`（`29,302` waves，**Round 5 新晋级 `LT_PV8=1`**：`272×8 + 72×16 = 3,328` WMMA cycles/wave）-> 纯 WMMA `~254.0 µs` vs **`834.1 µs`**（占比 `~30.4%`，`@idx` 单变量实省 `−12.1 µs`，`dec_up_64 −17.5 µs`、`dec_dfn_64 −21.1 µs`）。
- **边界声明**：`T_pred` 仅完成上述局部代表核的纯 WMMA 下界对账；全网 22 核尚缺动态循环/early-skip 统计、VALU/VMEM 重叠周期与硬件实际频率采样，暂不构造未测全网 `T_pred` 总式。

## 维度 5 — 核间屏障气泡、`FLOW` 终裁与边界

- **配对 GPU 时间戳拆分（`r5-final-flow01-idx-4k-{f0,f1}\openrr_candidate.log:156`，600f 均值）**：
  - **`FLOW=0`**（默认，`21` 核，中位 `10.5022 ms`）：21 核总和 `10,450.6 µs` + **派发间空隙和 `27.0 µs`** = 首尾总跨度 `10,477.6 µs`；
  - **`FLOW=1`**（`22` 核，中位 `10.4996 ms`）：22 核总和 `10,448.9 µs` + **派发间空隙和 `28.5 µs`**（含 `32:dec_dfn_96_fl -> 34:dec_up_64` `12.5 µs`）= 首尾总跨度 `10,477.4 µs`。*(注：在持久 `_fl_lt` 分支内，子波尾部与行标志自旋等待发生在单个 dispatch 内部，需未来交互式 AMD RGP 抓取波级时间线方可继续拆分。)*
- **无探针 `3×ABBA` `flowab`（`F1 − F0`，`max_dev <= 0.256%`）**：
  - `New=1 @ 4K` (`r5-final-flowab-new-4k`): `10.4818` vs `10.4850 ms`（`+3.17 µs`, med **`0.00 µs`**）
  - `New=1 @ 1080p` (`r5-final-flowab-new-1080p`): `2.7924` vs `2.8067 ms`（`+14.29 µs`, med **`+11.56 µs`**）
  - `Fast=2 @ 4K` (`r5-final-flowab-fast-4k`): `10.0988` vs `10.1116 ms`（`+12.85 µs`, med **`+12.20 µs`**）
  - `Fast=2 @ 1080p` (`r5-final-flowab-fast-1080p`): `2.6359` vs `2.6341 ms`（`−1.81 µs`, med **`−0.68 µs`**）
- **终裁**：`FLOW=0` 在无探针 4K/1080p `New` 与 4K `Fast` 均不慢于 `FLOW=1`，且 **Anchor #2 实证 `New=1` 与 `Fast=2` 的 `FLOW=0` vs `FLOW=1` 双档全部 `140/140` 帧 `max_ulp = 0`、`diff_halfs = 0`**；维持安全默认值 **`FLOW=0`**（`runtime.cpp:924`）。

## 维度 6 — 各通道宽度波数 / 驻留终表

| C | 核 | 线程 | 体 | VGPR | waves/SIMD | LDS | 变化说明 |
|---|---|---|---|---|---|---|---|
| 32 | `enc0`/`dec_up_32`/`dec_head_32` | 32 | W1 | 192 | 8 | 0–4 KB | 保留 `LT_Q16`/`LT_VO`（`W1A8` `216/240 VGPR` 双败归档） |
| **64** | `enc_64`/`dec_up_64`/`dec_dfn_64` | **64** | **W2 (`LT_PV8=1`)** | **240** | **6** | **9.7 KB** | `enc_64 LT_MRG8=1`；**Round 5 新晋级三核 `LT_PV8=1`**（400f `6/6` `−49.37 µs @4K` / `−15.49 µs @1080p`） |
| 96 | `enc_96`/`dec_dfn_96` | 64–128 | W2/TR | 256 | 6 | 9.7–25.6 KB | `enc_96 LT_MRG8=1`；`D6b C=96 PV8` 因 `−0.1431/−0.1921 dB` 与 1080p 退化归档 |
| **96 (up)** | **`dec_up_96_128_128`** | **64 (`gfx12`)** | **W2 (`W2_96UP=1`)** | **256** | **6** | **12.3 KB** | **仅限 `gfx12`**（`gfx11` 保持 `(128,1,1)`）；`23/24` 腿胜、`@idx 4K −46.0 µs` |
| 128 | `enc_128`/`dec_up_128`/`dec_dfn_128` | 128–256 | TR | 192–256 | 6–8 | 9.7–29.7 KB | **Round 5 400f 仲裁将 `dec_dfn_128 LT_PV8` 退回 OFF**；`C=128-up/enc PV8` 无效归档；非 `onr_block_w2` 新双波方案留待后续 |
| 160 | `enc_160×2`/`dec_up_160`/`dec_dfn_160` | 256 | TR | 192 | 8 | 30.2 KB | `D6b C=160 PV8` 因 3 核跳 `256 VGPR` 致 `4K +112.7 µs` 归档；`shared-cmat`/WGP 维持禁用 |

## 维度 7 — 图像壳物理带宽与非矩阵核终账（4K Qual）

AMD RX 9070 XT 官方显存带宽规格为 **`640 GB/s`**（20 Gbps × 256-bit）；按模型工作集估算最小理论流量 `~1.225 GB / 640 GB/s ≈ 1.914 ms`（`~1.91 ms` 下限）。终版 `r5-final-unshackle-idx-new-4k-f0` 非矩阵核实测：
- `post_f_lt = 1,226.7 µs`、`d3d_enc0_attr_f_lt = 858.1 µs`、`d3d_downsample_lt = 264.2 µs`（`global` 回退）、`d3d_ae = 56.1 µs`、`pooled_history2_lt = 38.2 µs`（`F1` 下 `clear_range = 0.3 µs`）。
- 图像壳主三核合计 **`2,349.0 µs`（约 `2.35 ms`）**，按估算最小流量计算的**表观有效带宽比 `1.914 / 2.349 ≈ 81.5%`**。*(声明：`~81.5%` 基于估算工作集流量得出；实际 DRAM 总线占用率与 `post_f_lt` ACO vs LLPC `+128.3 µs @4K / +24.5 µs @1080p` 的访存/缓存成因，尚需后续硬件性能计数器或交互式 RGP 实测验证，本文未宣称 RGP 抓帧。)*

---

## WP-D1～D6 逐项最终裁决矩阵

| 项 | Round 5 最终裁决 | 关键实测依据 |
|---|---|---|
| **D1a** (`enc_64`/`enc_96` `LT_MRG8` + UT8 尾段) | **晋级保留** | 4K 均值 `−55.4 µs`（`6/6`, 最好 `−104.3 µs`）；`enc_96 nomrg8` clean 隔离证实 `@idx +17.5 µs` / `4K ABBA +3.8 µs` (`2/6`) / `1080p +6.8 µs` (`1/6`) |
| **D1b** (`enc_160 MRG8_UNROLL`) | **归档回退** | `216 VGPR`、`40,448 B LDS`（破门限） |
| **D2a** (`dec_dfn_160` WGP / `shared-cmat`) | **归档回退** | `loadstore` 新哈希 `@idx −0.2 µs`、`4K ABBA +13.5 µs` (`1/6`)；DFN WGP `4K −1.2 µs` (`3/6` 噪声内) |
| **D2b** (`post_f_lt` WGP / `LT_POST_STILL`) | **归档回退** | Post WGP clean `4K +20.2 µs` (`2/6`)、`1080p` 含 `+783.6 µs` 毛刺；`LT_POST_STILL` `+10.3 µs` |
| **D3** (`W2_96UP=1` / `C=128 W2`) | **`W2_96UP=1` 晋级（仅 `gfx12`）**；`C=128 W2` 关闭 | 4 套无探针 `3×ABBA` **`23/24` 腿胜**；`@idx 4K −46.0 µs`（累计）；**`shaders-gfx11` 已显式剥离并锁定 `(128,1,1)`** |
| **D4** (`_lt`/`_lx` `native` + `FLOW=0`) | **已合入** | `37` 核 `native` + `4` 支 `downsample*_lt` `global` 安全回退（`Item NFMA`）；`flowab` + 双档 **`140/140` `0-ULP`** 锁定默认 `FLOW=0` |
| **D5** (`LT_W1A8`) | **归档回退** | `216/216/240 VGPR`，Anchor #2 `−0.2910 dB`（vs seal）/ `−0.3270 dB`（vs `d1a`），双分辨率无探针 `3×ABBA` 双负 `0/6`（`+316.8 µs / +73.9 µs`） |
| **D6.0 / D6a / D6b (`LT_PV8`)** | **`D6a C=64×3` 晋级；`dec_dfn_128` 退回 OFF；`C=96/128-upenc/160` 归档** | **`C=64`（`enc_64`/`dec_up_64`/`dec_dfn_64`）**：`240 VGPR / 0 scratch`，Anchor #2 `−0.0427 dB`（vs `nopv8`）/ `−0.0899 dB`（vs `4332470`），**400f `3×ABBA` `4K −49.37 µs (−0.469%, 6/6)`、`1080p −15.49 µs (−0.553%, 6/6)`**；`dec_dfn_128` 400f 仲裁 `1080p F0 +0.17 µs (3/6)` / `4K F1 +6.63 µs (2/6)` 退回 OFF；`C=96 −0.1431 dB`，`C=128-upenc 1080p +0.25 µs (3/6)`，`C=160` 跳 `256 VGPR` 退化 `+112.72 µs` |

## 第 8 项 — 最终发行门禁全量签收单（基于洁净终包 `f6dcc83`）

| 门禁 | 实测结果 |
|---|---|
| **洁净构建与 `296/296` 交付一致性** | ✅ `OPENRR_CLEAN_STAGE=1` 全新空目录重编完成；`build/aco-v015/stage/` 与 `final-pkg/stage/` 各 **`296` 文件、100% SHA-256 一致、`0` `.log` 残留**；`THIRD_PARTY.md` 标明 `Mesa 26.3.0 (0a6fa60a)` |
| **94/94 `.nrp` 与中断原子防呆** | ✅ `manifest.json` `records=94`、`failures=[]`、`94/94` `.nrp` 零孤儿、全核 `scratch_bytes == 0`；`.openrr-d3d12-v1.staging.<pid>` 临时目录全写+SHA-256 校验后再原子替换；**`50/50` Legacy `.nrp` 较 `pre-unshackle-9ebfb19` SHA-256 逐字节不变** |
| **Anchor #1 (`Legacy=0` vs 官方 `v0.1.6`)** | ✅ `r5-final-anchor1-{1080p,4k}` 覆盖 `steady`+`drs` **4 张 `phase_last_frame` 输出**，及 `r5-final-up016-abba-leg-{1080p,4k}` **`12/12` 腿末帧**，全部 **`max_ulp_diff = 0`、`100% exact`、`non_finite = 0`** |
| **Anchor #2 + `[P1]` 闭环（`New=1` / `Fast=2` / `FLOW` / `DEEP`）** | ✅ `r5-final-anchor2-{fast,exp}-{flow0,flow1}` + `exp-{u9-flow0,deep0-flow1}` 共 6 轮 **840/840 回读帧 `total_non_finite = 0`**；每轮 `v1` 实命中 5090 参考 `18/97`（`79 missing_ref`）+ `v2` `29/43`（`14 missing_ref`、2 曝光探针 ABI 跳过）：<br>• **`New=1` (`F0`)**：`Q_S0_4k_perf = 52.0823`, `Q_S0_1080p_dlaa = 52.4206`, `Q_S1_1080p_qual = 39.1105`, `Q_S1_4k_qual = 47.7323`, `Q_S2_1080p_qual = 40.6314`, `Q_S2_4k_perf = 41.1768`, `P00 = 49.1568 dB`（最差 delta：vs `4332470 −0.0899 dB`、vs 合流 `m2 −0.0600 dB`、vs `Stage8-Seal −0.0804 dB`）<br>• **`Fast=2` + `[P1]`**：默认 `ONR_LT_DEEP="2,4,2,4"` 下 `r5_exp_f0` (`P00=47.5665`, `Q_S1 f63=45.9930`) 仅相对同参数 `r5_u9_f0`（`47.6054`, `46.0813`）差 `−0.0389 / −0.0883 dB`；设 `ONR_RR_DEEP=0` 立即回到 **`48.5126 / 48.5272 dB`**（精确复现 Round 3 `openrr-trace.asi` `48.5386 / 48.4364 dB`，证实因 `build_runtime_trace.sh:9` 漏 `-DONR_LT_DEEP` 所致）<br>• **`FLOW=0` vs `FLOW=1` 逐位**：`New=1` 与 `Fast=2` 均经 `--compare-frames` 实测 **`140/140` 帧 `diff_halfs = 0, max_ulp = 0`**；同路径 `--dump-frames`/`--compare-frames` 自比对已验证拒绝（`RC=1`） |
| **`shaders-gfx11` 94 核与作用域** | ✅ `capability 4212/4213 = 0`、`Float8 = 0`；**`dec_up_96_128_128_{f,fl}_lt.spv` 为 `[128, 1, 1]`**（对比 `gfx12` `[64, 1, 1]`） |
| **4 套单测与策略套件** | ✅ `test_aco_build_policy.py` **`13/13 OK`**、`test_present_scope.exe` `EXIT=0`、`test_dxgi_queue.exe` `EXIT=0`、`test_asi_units.exe` `EXIT=0`（含 **`Item NFMA`** + **`Item NFMA Direct D3D12 guides`** + `ALL ASI UNIT TESTS PASSED`，零 TDR） |
| **冷装验收 + 语言字体 + PE 表** | ✅ `r5-final-cold-install` `fm=0/1/2` 共 `310` 次 ACO 命中（`50/130/130`）、覆盖 `94/94` SPV、`LLPC=0`；四语包（`English.txt/ja.txt/ko.txt/zh-CN.txt`）+ `NotoSansCJKsc-Regular.ttf`（`21,659,488 B`）；`openrr-amd.asi`（`3,396,738 B`）导出 `InitializeASI` / `OnrRrGetApi`，仅导入 `7` 洁净系统 DLL（`bcrypt/d3d12/KERNEL32/msvcrt/SHELL32/USER32/VERSION`） |
| **仓库与行尾纪律** | ✅ 单分支 `codex/mesa-263-comparison`，全改动 **100% LF（`CR=0`）**，`git diff --check` 干净，严格未 push |
