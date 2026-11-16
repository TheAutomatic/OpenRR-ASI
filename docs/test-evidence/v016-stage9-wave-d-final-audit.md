# OpenRR Stage 9 (Wave D) — 7 维理论上限终审与发行门禁签收（v016）

- 工作树/分支：`codex/mesa-263-comparison`（本轮终点 `23854ce`，父封板 `b26472a` = tag `stage8-seal-b26472a`；未 push）
- 最终全绿发行包：`build/aco-v015/stage/`（冻结副本 `build/v015-runs/stage9/final-pkg/`）
- 过程证据：`docs/test-evidence/v016-stage9-wave-d-progress.md`（第一轮 + 第二轮全部五项证据与归档记录）
- ISA 方法：全部采用 ACO 原生后端（`RADV_DEBUG=shaders`，截取 `After lowering to hw instructions:`）；裸 llvm-mc-21 已废止（GFX12 `v_cvt_pk_fp8_f32` 首字 `0xd769` 误码导致 4 字节错位，见进展文档 §4.1）

**晋级汇总**：WP-D1a（`MRG8_W` UT8 尾段偏移修复 + enc_64/enc_96 `LT_MRG8=1`）与 WP-D4 前半（`_f_lt` native 白名单）晋级合入；D6.0 XIN 两相拆平中性保留；其余候选（D1b、D2a/b、D3、D5/W1A8、D6a/b）全部按五项证据归档。

## 维度 1 — 全网算子精度与静态/动态 ISA 终表（ACO 原生后端）

22 个在跑核（4K Quality @idx 窗口）静态指令账（`v_wmma_f32_16x16x16_fp8_fp8` / `v_wmma_f16_16x16x16_f16` / `v_cvt_pk_fp8_f32` / `v_maxmin*` / `v_cndmask_b32` / `v_perm_b32` / `s_setreg*`；VGPR / LDS / scratch）：

| 核 | fp8 | f16 | cvt_pk_fp8 | maxmin | cndmask | perm | setreg | VGPR | LDS | scratch |
|---|---|---|---|---|---|---|---|---|---|---|
| pooled_history2_lt | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 24 | 0 | 0 |
| d3d_ae | 0 | 0 | 0 | 3 | 6 | 0 | 0 | 24 | 1024 | 0 |
| d3d_enc0_attr_f_lt | 0 | 0 | 0 | 57 | 237 | 46 | 0 | 96 | 0 | 0 |
| enc0_stage_32_64_f_lt | 96 | 116 | 192 | 0 | 24 | 24 | 14 | 192 | 0 | 0 |
| enc_stage_64_64_96_f_lt | 240 | 104 | 152 | 0 | 2 | 288 | 15 | 240 | 9728 | 0 |
| enc_stage_96_128_128_fl_lt | 408 | 118 | 160 | 0 | 0 | 240 | 7 | 256 | 25600 | 0 |
| enc_stage_128_128_160_fl_lt | 192 | 136 | 96 | 0 | 0 | 320 | 8 | 256 | 29696 | 0 |
| enc_stage_160_256_0_fl_lt | 40 | 36 | 84 | 0 | 0 | 80 | 6 | 192 | 30208 | 0 |
| enc_stage_160_256_160_fl_lt | 40 | 76 | 100 | 0 | 1 | 80 | 8 | 192 | 30208 | 0 |
| dec_up_160_256_160_fl_lt | 100 | 26 | 124 | 0 | 0 | 80 | 24 | 192 | 30208 | 0 |
| dec_dfn_160_256_fl_lt | 40 | 36 | 84 | 0 | 4 | 81 | 6 | 192 | 30208 | 0 |
| dec_up_128_128_160_fl_lt | 170 | 128 | 104 | 0 | 0 | 140 | 24 | 240 | 21504 | 0 |
| dec_dfn_128_128_fl_lt | 160 | 136 | 72 | 0 | 4 | 65 | 20 | 192 | 9728 | 0 |
| dec_up_96_128_128_fl_lt | 176 | 112 | 80 | 0 | 0 | 64 | 24 | 192 | 16384 | 0 |
| dec_dfn_96_128_fl_lt | 96 | 68 | 112 | 0 | 7 | 112 | 6 | 256 | 9728 | 0 |
| dec_up_64_64_96_f_lt | 176 | 96 | 128 | 0 | 0 | 48 | 28 | 240 | 9728 | 0 |
| dec_dfn_64_64_f_lt | 128 | 104 | 96 | 0 | 7 | 77 | 10 | 240 | 9728 | 0 |
| dec_up_32_64_64_f_lt | 48 | 132 | 80 | 0 | 8 | 32 | 12 | 192 | 4096 | 0 |
| dec_head_32_64_f_lt | 16 | 132 | 48 | 12 | 52 | 20 | 4 | 192 | 0 | 0 |
| post_f_lt | 0 | 0 | 0 | 12 | 103 | 0 | 0 | 96 | 8320 | 0 |
| d3d_downsample_lt | 0 | 0 | 0 | 12 | 10 | 0 | 0 | 48 | 2784 | 0 |
| clear_range | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 24 | 0 | 0 |

（数据源 `build/v015-runs/stage9/isa_dim1.json`：22 核批量 ACO 原生后端探针 + manifest。全部 `scratch=0`；16 个神经网络核中 15 个 `v_maxmin==0`，唯 `dec_head_32_64_f_lt` 余 12 条（其 head_px 的 sigmoid/tanh 数值钳位语义，非量化折叠断裂——该核 `v_cvt_pk_fp8_f32`=48 在场）；图像壳核（post/downsample/d3d_ae）的少量 maxmin 为其原生 min/max 语义。）

- **残留 f16 WMMA 说明**：C=32 的 116 条 = `LT_Q16/LT_VO` fp16 近似路径（本轮实测 FP8 全替换判负：画质 −0.29 dB / 性能 −2.8%，见进展文档 §4.3）；C≥64 的 66–104 条 = 精确 fp16 注意力（FP8 化按静态账本 264+ VGPR 必 spill + 纯增激活量化误差，整组归档，见 §7）。动态占比按 4K 窗口加权后 f16 WMMA 周期占比由 47.7% 降至约 44%（enc_64 MRG8 的 fp8 替换 + 各核 MI8/UP8 扩张）。
- `s_setreg` 集中在 `dec_up_64_64_96_f_lt`（28）——D6.0 两相拆平实测证明源码级不可再降（无屏障两相被调度器重排）。

## 维度 2 — 全 6 档预设稳态 vs RTX 5090（310.9.1.0）终表

默认实机态（`ONR_RR_FLOW=0`，warmup 64 / 400 帧稳态，同套件口径）：

| 预设 | Legacy=0 中位 | New=1 中位 | Fast=2 中位 | 5090 参照 | New 比 | Fast 比 |
|---|---|---|---|---|---|---|
| 1080p Quality | 3.6300 | 2.8370 | 2.7706 | 0.871 | 3.26x | **3.18x** |
| 1440p Quality | 6.1880 | 4.8728 | 4.7235 | 1.407 | 3.46x | **3.36x** |
| 4K Quality | 14.9514 | 10.8782 | 10.4988 | 2.915 | 3.73x | **3.60x** |
| 4K Performance | 8.8176 | 6.8425 | 6.6800 | 1.958 | 3.49x | **3.41x** |
| 1080p DLAA | 7.2429 | 5.6299 | 5.4492 | 1.545 | 3.64x | **3.53x** |
| 4K UltraPerf | 5.0984 | 3.9631 | 3.8916 | 1.287 | 3.08x | **3.02x** |

- 4K Qual 双口径（FLOW=1 @idx 600 帧）：纯神经网络 16 核和 ≈ 8.9 ms；整帧 22 核含 `attr/post/downsample` ≈ 10.5 ms（p1 终表 10541.9 µs / 22 核）。
- FLOW=1 流控态参考（4K Qual 无探针 3×交错）：New 中位 10.79 ms 档。

## 维度 3 — 同宿主同 SPIR-V ACO vs LLPC

未改核与 Stage 8 终表一致（SPIR-V 逐字节相同，`b26472a` 终审数据继续有效）；本轮仅 enc_64/enc_96（+`_f_lt` native 4 核码距变化）移动，ACO 侧配对（vs seal）：enc_64 **−37.9 µs**、enc_96 −1.0 µs，其余核带内 ±0.36%。历史逆差三核的最终位置：`post_f_lt` 1217 µs（+1.2 µs vs p1，WGP 实验证明不可收）、`dec_dfn_160_256_fl_lt` 86.2 µs（WGP/shared-cmat 双实验皆无收益）、`d3d_enc0_attr_f_lt`（本轮未触及，维持 Stage 8 结论）。

## 维度 4 — Mochi 加法周期模型对账（4K Qual）

`128 SIMD @ ~3.0 GHz = 384,000 SIMD-cycles/µs`。以 enc_stage_64_64_96_f_lt 为例：960×540 内部画布 / 64-token 窗口 = 8,100 窗口 × 每窗口静态 `240×8 + 104×16 = 3,584` WMMA 周期 ≈ 75.6 µs 纯 WMMA 下限，实测 860 µs → WMMA 占比 ~8.8%，其余为访存/VALU/softmax/驻留等待（240 VGPR = 6 waves/SIMD 驻留）。同法 dec_dfn_160_256：480×270/64=2,025 窗 × (160×8+84×16)=2,624 周期 ≈ 13.8 µs WMMA 下限 vs 实测 86 µs（16%）——大 C 核普遍为访存/发射受限，与"FP8 化收益有限"的实测互相印证。

## 维度 5 — 核间屏障气泡与 FLOW 终裁

| 口径 | 22 核和 µs | 核间空隙 µs | 稳态中位 |
|---|---|---|---|
| STAMPS=1 FLOW=1 | 10536.8 | 175.0 | 10.6674 ms |
| STAMPS=1 FLOW=0 | 10561.7 | 66.4 | 10.6120 ms |
| **无探针 3×交错 FLOW=1** | — | — | 10.79/10.74/10.83 ms |
| **无探针 3×交错 FLOW=0** | — | — | 10.82/10.82/10.88 ms |

**终裁**：无探针（权威口径）下 4K `FLOW=1` 快 **−49.1 µs**（中位），1080p 中性（+3.2 µs）——STAMPS 探针查询本身串行化扭曲了第一轮"F0 快 55 µs"的读数。宿主默认维持 `FLOW=0`（`runtime.cpp:924`，安全默认），两套水位如上双列，不混标。

## 维度 6 — 各通道宽度波数/驻留终表

| C | 核 | 线程 | 体 | VGPR | waves/SIMD | LDS |
|---|---|---|---|---|---|---|
| 32 | enc0/dec_up_32/dec_head_32 | 32 | W1 | 192 | 8 | 0–4 KB |
| 64 | enc_64/dec_up_64/dec_dfn_64 | 64 | W2 | 240 | 6 | 9.7 KB |
| 96 | enc_96/dec_up_96/dec_dfn_96 | 64–128 | W2/TR | 240–256 | 6 | 9.7–25.6 KB |
| 128 | enc_128/dec_up_128/dec_dfn_128 | 128 | TR | 240–256 | 6 | 21.5–29.7 KB |
| 160 | enc_160×2/dec_up_160/dec_dfn_160 | 128–256 | TR | 192–216 | 7–8 | 21.5–30.2 KB |

W2_96UP（dec_up_96 → 2-wave）实测自身 −10.0 µs 但下游 dec_dfn_96 +21.6 µs（输出布局耦合），结队净负回退；C=128 2-wave 静态账本必破 256 VGPR，按条款不盲跑。

## 维度 7 — 图像壳物理带宽 Roofline（4K Qual）

最小唯一内存流量 ≈ 1.225 GB/帧（enc0_attr 读 2560×1440×48 B 指南 + post 5760×3240 双向 + downsample 5760×3240→3840×2160），644 GB/s 针脚上限对应 **1.90 ms** 下限。实测图像壳三核和（attr 0.86 + post 1.22 + downsample 0.26 ms）≈ 2.34 ms → **带宽利用率 ≈ 81%**（含重复采样与 LDS 中转的额外流量后真实利用率更高）。

## 第 8 项 — 发行门禁签收单

| 门禁 | 结果 |
|---|---|
| 94/94 `.nrp`（零冗余、`scratch=0`） | ✅ records=94 failures=0，全核 scratch=0 |
| Anchor #1 `Legacy=0` vs ref-016-release | ✅ 4K + 1080p steady+DRS `max_ulp_diff = 0`（100.0000% 0-ULP） |
| Anchor #2 `New=1`（fast-aco） | ✅ v1 29/29 全 ≥0（best +0.0307 dB）、v2 21/21 最差 −0.1415 dB、nf=0 |
| Anchor #2 `Fast=2`（exp-aco） | ✅ vs exp 基线 v1 最差 +0.0000（best +0.0406）、v2 最差 −0.1217、nf=0 |
| Anchor #2 `FLOW=0`（默认态对账） | ✅ 与 FLOW=1 逐值一致（_f_lt/_fl_lt 画质对齐验证） |
| `shaders-gfx11` 94 核 | ✅ capability 4212/4213 = 0、`Float8` 字符串 = 0 |
| 4 套测试 | ✅ policy 9/9、present_scope EXIT=0、dxgi_queue EXIT=0、asi_units 全 PASS（含 WP-D1/D5 新断言） |
| 空白目录冷装验收 | ✅ Temp 空目录冷装 24 帧 EXIT=0（manifest 路径核验） |
| 四语包 + CJK 字体 + PE 导入表 | ✅ English/zh-CN/ja/ko + NotoSansCJKsc-Regular.ttf；导入表 bcrypt/d3d12/KERNEL32/msvcrt/SHELL32/USER32/VERSION 干净 |
| 全仓 LF 行尾 | ✅ `git diff --check` 干净；改动文件 100% LF（Step 0 已还原 CRLF 污染） |
| 分支纪律 | ✅ 单分支 `codex/mesa-263-comparison`，工作树干净，未 push |
