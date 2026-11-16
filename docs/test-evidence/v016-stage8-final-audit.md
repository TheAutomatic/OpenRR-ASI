# OpenRR v0.1.6 + Stage 8 终局审计报告（2026-11-16 晨，`codex/mesa-263-comparison` 代码封板 `e745d71`）

依据：`docs/test-evidence/v016-stage8-phase1-merge-baselines.md`（Phase 1）、`v016-stage8-phases2-6-results.md`（Phase 2–6 修复与负例处置）。全部终验均基于同一个封板构建产物 `build/aco-v015/stage`（代码头 `e745d71`），原始输出目录位于 `build/v015-runs/stage8/final-remed-*` 与 `final-llpc-*`。

---

## 维度 1 — 终版 manifest 全核静态资源与指令档位（`94/94` spv → `.nrp`，`scratch==0`）

来自 `build/aco-v015/stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.json`（实测 `records=94`、disk `.nrp=94`、`failures=[]`、`cu_mode=False`、`wgp_mode=False`、`sample_wait_zero=False`）：

| 内核家族 | 代表 `_lt` / `_lx` / 常驻记录 | VGPR | LDS (B) | scratch | exec_bytes | 说明 |
|---|---|---|---|---|---|---|
| 属性提取 | `d3d_enc0_attr_f_lt` / `f_w_lt` | 96 | 0 | 0 | 22524 / 29064 | **WP-C5 `LT_ATTR_RCP=1`（`09246e0`，−4,076 B）** + 位级 NaN（native-only） |
| 编码 W1 (`32→64`) | `enc0_stage_32_64_f_lt` / `f_lx` | 192 / 192 | 0 | 0 | 24372 / 23860 | `LT_MLP8` + `LT_EMB8`；`_lx` + `W1_GELU8` 线性门 |
| 编码 W2 (`64→96`) | `enc_stage_64_64_96_f_lt` | 240 | 9728 | 0 | 25340 | `MI8` + `MLP8` + `WP-C1 FP8QV` + `WP-C4 E8RAW` |
| 编码 (`96→128`) | `enc_stage_96_128_128_f_lt` / `fl_lt` | 256 / 240 | 12288 / 19456 | 0 | 21156 / 36916 | `MLP8` + `E8RAW`（exec −4,424 / −2,396 B）|
| 编码 (`128→160`) | `enc_stage_128_128_160_f_lt` / `fl_lt` | 256 / 256 | 29696 | 0 | 27856 / 29184 | `MLP8` + `MRG8` + `E8RAW`（exec −8,396 / −8,460 B）|
| 编码 (`160→256`) | `enc_stage_160_256_160_fl_lt` / `0_fl_lt` | 192 | 30208 | 0 | 19856 / 14724 | `MLP8` + `E8RAW`；CU 模式 |
| 解码上采样 (`160`) | `dec_up_160_256_160_fl_lt` | 192 | 30208 | 0 | 19276 | `UP8` + `MLP8` + `WP-C1 FP8QV` + `E8RAW`，`SPLIT=2` |
| 解码平滑 (`160`) | `dec_dfn_160_256_fl_lt` | 192 | 30208 | 0 | 15676 | `MLP8` + `E8RAW` |
| 解码上采样 (`128`) | `dec_up_128_128_160_fl_lt` | 240 | 21504 | 0 | 24068 | `FP8QV` + `MLP8` + `E8RAW` |
| 解码平滑 (`128`) | `dec_dfn_128_128_fl_lt` | 192 | 9728 | 0 | 22304 | `MLP8` + `E8RAW` |
| 解码上采样 (`96`) | `dec_up_96_128_128_fl_lt` | 192 | 16384 | 0 | 22212 | `UP8` + `FP8QV` + `MLP8` + `E8RAW` |
| 解码平滑 (`96`) | `dec_dfn_96_128_fl_lt` | 256 | 9728 | 0 | 17780 | `MLP8` + `E8RAW` |
| 解码上/平 W2 (`64`) | `dec_up_64_64_96_f_lt` / `dec_dfn_64_64_f_lt` | 240 / 240 | 9728 / 9728 | 0 | 21604 / 19120 | `UP8` + `MLP8` + `WP-C1 FP8QV` + `E8RAW` |
| 解码上采样 W1 (`32`) | `dec_up_32_64_64_f_lt` / `f_lx` | 192 / 192 | 4096 | 0 | 16136 / 15608 | `Q16/VO/SYM`；`_lx` **VGPR 216→192** |
| 解码头部 W1 (`32`) | `dec_head_32_64_f_lt` / `f_lx` | 192 / 192 | 0 | 0 | 20856 / 18408 | `Q16/VO/SYM` + `MLP8` + `E8RAW` |
| 后处理 | `post_f_lt` / `post_dlt_f_lt` | 96 / 96 | 8320 / 9984 | 0 | 15828 / 21308 | FP32 累加 + native-only |
| 下采样 / 统计 / 杂项 | `d3d_downsample_lt` / `_tl_lt` / `d3d_ae` / `pooled_history2_lt` | 48 / 96 / 24 / 24 | 2784 / 6016 / 1024 / 0 | 0 | 1932 / 16420 / 1740 / 964 | `TLLDS`（v0.1.6） |

**全包 94 条记录 `scratch_bytes == 0`**。保留 FP16 的算子依据：`W1` Q/V/VO（`WP-C2 2A` 踩 `216` 台阶 + LR `−1.07 dB`；`2B/2C LT_VO8` 循环开销 `+232.8 µs` + Anchor #2 `−3.04 dB`）；`DFN_PRE`（消费 `x2^T`，`N=16` 单发不回本）；`W_out/S/O`（局部 tile 寄存器通路，非 WMMA 带宽瓶颈）。

---

## 维度 2 — 三档模式终局稳态性能与 RTX 5090 同套件比值

基准参照：RTX 5090（同 Windows D3D12 `RR_Contrast` + `310.9.1.0`）：**1080p Qual `0.871 ms`**、**4K Qual `2.915 ms`**。终验长测口径：`1500w+600f`（1080p）、`500w+600f`（4K），显式 `ONR_RR_FLOW=1`。

| 模式 | 1080p 稳态中位 (均值 / p95) | 4K 稳态中位 (均值 / p95) | vs RTX 5090 (1080p / 4K) | `non_finite` |
|---|---|---|---|---|
| **Legacy=0** (`final-remed-steady-*-f0`) | `3.60156 ms` (`3.62506 / 3.76672`) | `13.57640 ms` (`13.58530 / 13.91520`) | `4.13x` / `4.66x` | `0` |
| **New=1** (`final-remed-steady-*-f1`) | **`2.84732 ms`** (`2.84660 / 2.88196`) | `11.09600 ms` (`11.12540 / 11.60780`) | **`3.27x`** / `3.81x` | `0` |
| **Fast=2** (`final-remed-steady-*-f2`) | **`2.78768 ms`** (`2.78553 / 2.82228`) | **`10.51570 ms`** (`10.53430 / 10.83560`) | **`3.20x`** / **`3.61x`** | `0` |

- **Stage 8 合流后累计净推进（`New=1` 3×ABBA 锁定交错消除热/频漂移，`final-remed-abba-*` vs `frozen-016-base`）**：
  - **4K**：均值 **`−2.6766%`**（中位 `−2.6291%`，**`6/6` 负**，腿差 `[−2.8792, −2.5862, −2.6578, −2.5582, −2.7781, −2.6004]%`，交错均帧时 **`10.78183 ms vs 11.07838 ms`** = **`3.70x` RTX 5090**）。
  - **1080p**：均值 **`−3.1549%`**（中位 `−3.1682%`，**`6/6` 负**，腿差 `[−3.2399, −3.0789, −2.9897, −3.0965, −3.2673, −3.2570]%`，交错均帧时 **`2.83242 ms vs 2.92469 ms`** = `3.25x` RTX 5090）。
  - （拆解构成：4K 合计 `-2.68%` ≈ `WP-C1 (-0.32%)` + `WP-C4 LT_E8RAW (-1.52%)` + `WP-C5 LT_ATTR_RCP (-0.62%)`；1080p 合计 `-3.15%` ≈ `WP-C1 (-0.38%)` + `WP-C4 (-1.93%)` + `WP-C5 (-0.71%)`；三项独立 ABBA 之和与端到端交错 ABBA 高度自洽。）
- **@idx 4K 端到端全核总和（`final-remed-idx-4k` vs `frozen-016-base`）**：`10869.8 -> 10582.0 µs`（**`−287.8 µs / −2.65%`**）。

---

## 维度 3 — 终局同宿主同 SPIR-V ACO vs 驱动 LLPC（对照③，`New=1`）

使用同一封板宿主 `e745d71`、同一组 SPIR-V 仅切换 `--candidate-compiler llpc` vs `aco-strict`：

- **帧级 3×ABBA（`final-llpc-abba-*`）**：
  - **4K**：LLPC 慢 **`+21.6304%`**（中位 `+21.4901%`，**ACO 赢 `6/6` 腿**，`13.08905 vs 10.76138 ms`）。
  - **1080p**：LLPC 慢 **`+20.1615%`**（中位 `+20.2191%`，**ACO 赢 `6/6` 腿**，`3.40620 vs 2.83469 ms`）。
- **核级 `@idx 4K`（`final-llpc-idx-4k`）**：paired total **ACO `10588.3 µs` vs LLPC `12836.6 µs`（LLPC 慢 `+2248.3 µs / +21.23%`）**。
  - **ACO 最大领先核**：`enc0_stage_32_64_f_lt` `1330.9 vs 1980.4 µs`（LLPC `+48.80%`）、`dec_up_32_64_64_f_lt` `+323.9 µs (+28.89%)`、`dec_head_32_64_f_lt` `+261.1 µs (+22.12%)`、`dec_up_64_64_96_f_lt` `+256.7 µs (+34.25%)`、`enc_stage_64_64_96_f_lt` `+234.0 µs (+25.98%)`、`dec_dfn_64_64_f_lt` `+192.4 µs (+28.19%)`、`dec_up_96_128_128_fl_lt` `+106.0 µs (+24.58%)`、`dec_dfn_128_128_fl_lt` `+89.4 µs (+60.41%)`。
  - **LLPC 仅余小幅领先核（供 Wave D 参考）**：`post_f_lt` `1229.2 vs 1103.4 µs`（`−125.8 µs / −10.23%`）、`d3d_enc0_attr_f_lt` `858.5 vs 840.1 µs`（`−18.4 µs / −2.14%`，已由 `-75 µs` 级大幅收窄）、`dec_dfn_160_256_fl_lt` `87.6 vs 69.9 µs`（`−17.7 µs / −20.21%`）、`d3d_ae` `56.5 vs 52.5 µs`（`−4.0 µs`）、`d3d_downsample_lt` `264.4 vs 263.4 µs`（`−1.0 µs`）。

---

## 维度 4 — 周期模型与假设对账

- **成立并命中预测的改动**：`WP-C1`（一次量化复用消减 `XIN` 重复转换）、`Phase 4 Fast=2 GELU`（去 4-op 多项式降 VGPR 216→192 且降 `-1.8%`）、`WP-C4 LT_E8RAW`（去全链 `e4m3` 次正规软件解码，砍掉高达 `8.5 KB`/核 VALU 指令并实测 `-1.52%/-1.93%` 且 Anchor #2 反升）、`WP-C5 LT_ATTR_RCP`（倒数化简 `enc0_attr` 砍 `-4 KB` 并降 `-8.11%` 核时）。
- **被实验推翻的静态预期**：`WP-C2 2A/2B LT_VO8`（2A 踩 216 VGPR 台阶且 LR 非 e4m3 精确；2B 通过 `dont_unroll` 守住 192 VGPR 仍因指令串行化退化 `+2.22%` 且 `P00 -3.04 dB`）；`C=160` 与全局 `--wgp-mode-shaders`（RDNA4 WGP 模式下同步/调度反而拖累 160 族 +12%~+16%）；`post_f_lt` `--sample-wait-zero-shaders`（+0.2 µs，不受该 waitcnt 限制）与 `[[dont_unroll]]`（+3.22%，破环双像素交错调度）。

---

## 维度 5 — 核间屏障与邮戳间隙普查（`final-remed-idx-4k`）

依据 `idx_report.py`（通过 `@2`/`@42` 索引断言校验：`ints[idx]` 为核自身耗时，`ints[idx+1]` 为该核后的间隙，596 稳态帧均值）：
- 22 个已标记派发总核时 = **`10582.0 µs`**，22 处核后间隙总和 = **`229.4 µs`**（占整网 `10.81 ms` 的 **`2.1%`**；对比基线 `frozen-016-base` 为 `226.2 µs`）。
- 唯二显著间隙：① **`32:dec_dfn_96_128_fl_lt -> 34:dec_up_64_64_96_f_lt`**（`FLOW` 与 `W2` 衔接尾隙）= **`116.5 µs`**（基线 `120.5 µs`）；② **`42:post_f_lt -> 44:d3d_downsample_lt`** = **`102.5 µs`**（基线 `40.9 µs`；前序 `dec_head_32_64_f_lt@40 -> post_f_lt@42` 同步对应由 `53.0 µs` 降至 `1.5 µs`）；其余 20 处间隔全部为 `0.0–2.9 µs`。
- 波级原因（尾波饥饿 vs 屏障排空 vs 队列调度）需目标机交互式 AMD RGP 取证（Wave D）。

---

## 维度 6 — 波数划分与驻留甜点

- GFX12 `num_physical_wave64_vgprs_per_simd = 768`（双倍物理分配对应每 CU `1536` VGPR，最大 `16` waves/SIMD）：`96 VGPR`（`attr`/`post`/`downsample_tl`）达到满编 **`16 waves/SIMD`**；`192 VGPR` 处于 8–12 wave 档；W2/96/128/160 家族维持经 `C6`/`WGP` 检验的最优 CU 模式与原工作组配置。全包 zero scratch。

---

## 维度 7 — 非矩阵核专项贡献（`final-remed-idx-4k`，4K `network = 10.81 ms`）

| 核 | `@idx 4K` 耗时 | 占整网比 | VGPR / LDS / exec | Stage 8 处置结果 |
|---|---|---|---|---|
| `post_f_lt` | `1227.1 µs` | `11.4%` | 96 / 8320 B / 15828 B | `dont_unroll (+3.22%)` 与 `--sample-wait-zero (+0.2 µs)` 已证伪保持原调度 |
| `d3d_enc0_attr_f_lt` | **`858.2 µs`** | `7.9%` | 96 / 0 B / **22524 B** | **`LT_ATTR_RCP=1` 提速 −72.3 µs（`d2` 独测 −75.4 µs / −8.11%），与 LLPC 差缩至 2.1%** |
| `d3d_downsample_lt` | `264.3 µs` | `2.4%` | 48 / 2784 B / 1932 B | 与 LLPC 基本持平（`264.4 vs 263.4 µs`） |
| `d3d_ae` | `56.5 µs` | `0.5%` | 24 / 1024 B / 1740 B | 稳定 |
| `pooled_history2_lt` | `38.2 µs` | `0.4%` | 24 / 0 B / 964 B | 稳定 |
| （变体）`d3d_downsample_tl_lt` | *(可选引导激活时)* | — | 96 / 6016 B / 16420 B | v0.1.6 `TLLDS` 已合入并生成 ACO 记录 |

---

## 维度 8 — 终局发行门禁全量验收单（全部于封板头 `e745d71` 重跑实测通过）

| 门禁 / 验收项 | 终验结果 |
|---|---|
| **静态与包结构** | `94/94` `.nrp`（`failures=[]`，无滞留探针文件）；`Legacy` 记录较 `frozen-016-base` **0 字节差异**；`shaders-gfx11` 94 核零 capability `4212`/`4213` 与零 `Float8` 字符串；PE 导出 `1` 组（8 接口）/ 7 DLL（`bcrypt/d3d12/KERNEL32/msvcrt/SHELL32/USER32/VERSION`）；四语 `lang/{English,zh-CN,ja,ko}.txt`（71/71 词条 100% 对齐）+ `fonts/{NotoSansCJKsc-Regular.ttf,Noto-CJK-LICENSE.txt,README.txt}` |
| **4 项单测与策略套件** | `test_aco_build_policy.py`（**7/7 OK**）、`test_present_scope.exe`（**EXIT=0**）、`test_dxgi_queue.exe`（**EXIT=0**）、`test_asi_units.exe`（**31 条 `[PASS]`，`ALL ASI UNIT TESTS PASSED`，含 `Item OCP`、`Item LANG`**） |
| **Anchor #1（Legacy=0 vs 原生 `7e1df92` LLPC）** | 4K steady+DRS & 1080p steady+DRS 全部 **`100% 0-ULP`（`psnr_01=999`、`psnr_tonemap=999`、`non_finite=0`、`parity_passed=true`）** |
| **Anchor #2（`New=1` vs RTX 5090 `310.9.1.0`）** | **`v1` `76/76`（全用例最低 `Q_S0_4k_perf = 52.0809 dB`，`Q_S0_1080p_dlaa = 52.4199 dB`）+ `v2` `44/44`（全用例最低 `Q_S1_1080p_qual = 39.1378 dB`，`P00 = 49.1483 dB`，`Q_S2_1080p_qual = 40.6587 dB`）、`nf=0`**；相对 Phase D (`09246e0`) **逐帧差 `0.0000 dB`**；相对 `frozen-016-base` `v1` 最低 `+0.0441 dB` / `v2` 最差 case min `−0.0069 dB`；相对固定 `4332470` `v1` **`+3.7934..+20.3874 dB`** / `v2` 最差 case min **`−0.0626 dB`** |
| **Anchor #2（`Fast=2` `exp-aco` 时间域）** | **`v1` `76/76`（最低 `51.4244 dB`）+ `v2` `44/44`（最低 `38.9676 dB`）、`nf=0`** |
| **空白目录冷装验收** | `%TEMP%\openrr-stage8-cold-install` 独立目录完整校验 + `openrr_bench` @1080p（`48w+64f`）`2.813 ms`、`94/94` ACO 记录命中、`EXIT=0` |
| **仓库卫生与交接** | `codex/mesa-263-comparison` 工作树干净、未执行 `git push`、`*.ttf` 未入库 |
