# Stage 2 第二层收官：XS_TRIM 128 家族探针（+）+ LT_UP8 深层 FP8（负，回退留档）+ units 越界修复

分支 `codex/mesa-263-comparison`（`8fedb00` 之上）。第二层铁律：只动 `_lt` 变体；非 `_lt` SPIR-V 零变化 + 1 次 Legacy 0-ULP 防回归。

## 1. Step 1：`C=128` `_lt` 开启 `-DXS_TRIM=1` —— **保留（胜者）**

`onr_block.glsl:294-314` 的 `XS_TRIM` 分支上游早已实现，仅因当年 LLPC 略慢被排除 `ENC_C == 128`，从未在 ACO 实测。`fastmode.cmake` 对 `LT_DEFS_dec_dfn_128_128` / `LT_DEFS_dec_up_128_128_160` 追加 `XS_TRIM=1`（仅 `_lt` 生效）。

### 静态（`_f_lt`/`_fl_lt` × 2 核，vs `8fedb00`）

| 核 | LDS | VGPR | exec | scratch |
|---|---|---|---|---|
| `dec_dfn_128_128_f_lt` | 26112 → **9728** | 256 → **192** | +96 B | 0 |
| `dec_dfn_128_128_fl_lt` | 26112 → **9728** | 256 → **192** | +88 B | 0 |
| `dec_up_128_128_160_f_lt` | 32768 → **21504** | 256 → 240 | +108 B | 0 |
| `dec_up_128_128_160_fl_lt` | 32768 → **21504** | 256 → 240 | +4 B | 0 |

`dec_dfn_128` 解除 256-VGPR 封顶（192 VGPR + 9.5 KB LDS → WGP 波数上限大幅放宽）；`dec_up_128` LDS 上限 4w→6w/SIMD。

### 门禁与测速（vs 父基线 `v015-s1-fix`，3×32/120/64 ABBA）

- **0 ULP**：steady+DRS exact 100.0000%（XS_TRIM 只改 LDS 布局宏，不改任何运算）
- 速度：**−4.70% @1080p / −3.52% @4K**（此前 `8fedb00` 为 −2.80/−1.63，增益近翻倍）

## 2. Step 2：`C >= 96` DEC_UP 原生 FP8（`LT_UP8`）—— **判负回退，全链留档**

### 2.1 权重侧（正结果，已保留在最终提交中）

`engine.cpp:303` 放宽 `if (s.b.C <= 64) U8(...)` → `if (!s.up_w.empty()) U8(...)`：全部深层 `s.up_w` 均通过 `qv_bytes()` 的 254 精确 E4M3 校验（零异常）——**深层 up_w 免费获得零量化误差的精确 E4M3 尾段**（fp16 前缀不动，Legacy 不受影响）。该放宽同时是 §4 units 修复的载体，最终保留。

### 2.2 着色器侧（五个变体的完整取证链）

TR 体 DEC_UP 乘积（`onr_block_fast.glsl` `#elif DEC_UP` 段）依次试过五种实现：

| # | A 操作数 | B 操作数 | 画质 vs 父 | 结论 |
|---|---|---|---|---|
| v1 | e4m3 shared 暂存（连续布局 + `lds4` 索引 `>>3` 错） | `E4View` 尾段 COL 直读 | 28.77 dB | 布局 + 索引双错 |
| v2 | e4m3 shared 暂存（镜像 put16 分裂跑道） | 同上 | 28.77 dB（逐位同 v1） | 揭示问题不在 staging |
| v3 | fp16 LDS tile + `QA8()` FConvert | 尾段 COL 直读 | 29.44 dB* | FConvert coopmat 为 Mesa 冷门路径 |
| v4 | 同 v3 | fp16 全局 COL + `QB8()` FConvert | 29.44 dB（逐位同 v3） | 双 FConvert 恒定垃圾 |
| v5 | 元素级 `floate4m3_t(a_[i])` | 元素级 `floate4m3_t(b_[i])` | 29.44 dB（逐位同 v3/v4） | 排除转换算子 |

\* v1/v2 的 28.77 与 v3-v5 的 29.44 差异来自 **陈旧 runtime**（见 2.4），非着色器差异。

**判别实验**：LLPC 直跑同一 spv = 同样 29.44 dB → 排除 ACO 捕获/回放，锁定 GLSL 逻辑错。

**根因**（v5 逐位同 v3/v4 的唯一解释——输出不依赖乘积）：bias 朝向错。`bias_acc_t`（`onr_block_fast.glsl:245`）是 **COL** 装载、专用于 W2 的**转置累加器**；TR 的 UP 累加器朝向相反，fp16 路径用的是 **ROW** 的 `bias_acc`（`onr_block.glsl:609`）。从 W2 抄 `bias_acc_t` 未转回 → bias 加到像素轴而非通道轴。修复一行后 **59.73 dB**（exact 47.17%、ulp 58）。

59.73 的残差 = `v_wmma_f32_16x16x16_fp8_fp8` 与 fp16 版 WMMA 的内部求和树顺序差（A/B 数值均精确 e4m3，逐元素无损）。

> **勘误（`7ca278a` 次轮复核）**：v5 并非"原生 FP8"路径——A/B 仍以 16-bit FP16 从 LDS 与全局装载（**带宽一点没省**），且最内层 `(tile, k)` 循环对每个 16×16×16 tile 做 32 次标量 `floate4m3_t()` 转换，约倒贴 48-72 VALU 周期去省 8 个 WMMA 周期。因此 §2.3/§3 的正确结论是 **"v5 式带内层标量转换的 FP8 必然净负"**，而非"硬件 FP8 WMMA 慢于 FP16"——真原生 FP8（权重离线 1-byte 装载 + 激活在 N-tile 外一次性量化复用，零内层转换）的账另算，见 Stage 3。

### 2.3 速度判决（叠加在 XS_TRIM 之上，3×32/120/64 ABBA）

| 配置 | 1080p | 4K |
|---|---:|---:|
| 仅 XS_TRIM | **−4.70%** | **−3.52%** |
| XS_TRIM + UP8 | −3.80% | −2.54% |
| → UP8 净增量 | **+0.90pp 变慢** | **+0.98pp 变慢** |

fp8 WMMA 省的周期 < 每 tile 32 次元素量化的寄存器开销。**画质 59.73 dB（差 60 dB 线 0.27）且净变慢 → 按协议回退**。回退物：`fastmode.cmake` 三核 `LT_UP8` 移除、`onr_block_fast.glsl` 加 `#error` 硬闸（防止误启）、`engine.cpp` 保留 U8-all（§4）。

### 2.4 过程中发现并修复的两个基础设施坑（对后续所有工作生效）

1. **`build/aco-runtime-trace` 陈旧 runtime**：bench 用 `build/aco-v015/openrr-trace.asi`，之前是 02:15 的旧二进制——**engine.cpp 改动不重编 runtime 就全白测**。已重编并同步；`build_runtime_trace.sh` 默认 base（`build/asi`，11-11 对象）可用，`OPENRR_TRACE_BASE=build/asi-015-aco` 不可用（base 对象与 runtime 编译 flags ABI 不配 → 设备移除）。
2. **误跑 `git stash` + 陈旧 cherry-pick 状态**：已当场恢复（工作树三文件无损），sequencer 残留已清。

## 3. Step 3：`ENC_MERGE` 原生 FP8（`LT_MRG8`）—— **按证据跳过**

UP8 v5 的直接实测净 **−0.9~−1.0pp**——但 v5 带 32 次/tile 的内层标量转换且未省任何 16-bit 装载带宽（见 §2.2 勘误），其负收益**不能外推到真原生 FP8**。MRG8 若照 v5 模式做必然同负；若做真原生（Mochi 式激活外层量化 + 1-byte 权重直读）需重开账本，与 Stage 3 MLP FP8 一并评估。若 Stage 3 纯 FP8 方案需要重开：enc1 的 `LT_MI8` 产线路径（`MI_W` 尾段 ROW 直读 + `REC2Y` 寄存器构造）已验证可运行，可作起点，但需先解决"fp8 WMMA 实测慢于 fp16"这一根本问题（可能方向：更高 K 复用率摊薄量化开销、或 `LT_E8RAW` 直读消除 REC2Y）。

## 4. units 设备移除（0x887a0006）取证与修复 —— **已修复**

最终配置下 `test_asi_units` 在引擎条目（装载完 flow ladder 后首次提交）稳定设备移除，parity 全链路却正常。二分定位：

| engine.cpp | units | parity |
|---|---|---|
| 原版（深层 up_w 仅 fp16，2 B/elt） | **设备移除 ×4** | 过 |
| U8-all（深层 up_w fp16+e4m3，3 B/elt） | **ALL PASSED** | 过 |

机理：units 权重 blob 布局下，某深层 `dec_up` B-tile 读取越过 fp16-only `up_w` 缓冲末端、落出 blob 映射页（bench 真实权重的 blob 布局不触发）。尾段使越界读回到合法页内。**最终提交保留 U8-all**：fp16 前缀逐位不变（Legacy/Fast 0-ULP 均在 U8-all 引擎上验证通过），代价为深层 up_w 缓冲 +50%（整 blob 约 +1-2 MB），并为 Stage 3 纯 FP8 预置尾段。`engine.cpp` 内注释记录该实证。

## 5. Step 4 终验门禁（最终配置，`test_asi_units` + 双 0-ULP + 非 lt SPIR-V 零变化）

| 门禁 | 结果 |
|---|---|
| `test_asi_units` | **ALL PASSED** |
| 非 `_lt`（Legacy）SPIR-V vs `8fedb00` | 4 组抽查（dec_up_96/128、dec_dfn_128、enc_stage_160 的 `_f`/`_fl`）**SHA-256 逐字节一致** |
| Legacy-ACO vs 冻结基线（steady+DRS） | exact 100.0000% / ulp 0 |
| Fast-ACO vs 父构建（steady+DRS） | exact 100.0000% / ulp 0 |

### 终局配对测速（3×48/240/64 ABBA，vs 同提交 Fast-LLPC）

| 档 | Fast-ACO | Fast-LLPC | 差距 | `3b23cef` 起点 | `8fedb00` |
|---|---:|---:|---:|---:|---:|
| 1080p | 3.175 ms | 3.066 ms | **+3.56%** | +11.61% | +5.23% |
| 1440p | 5.389 ms | 5.163 ms | **+4.38%** | +10.08% | +4.14% |
| 4K | 11.801 ms | 11.382 ms | **+3.68%** | +8.29% | +3.92% |

### 单实例独立毫秒数（无 `--baseline-runtime`，对照 `Fast-LLPC` 交付门禁线 3.009 / 11.389 ms）

| 配置 | 1080p | 4K |
|---|---:|---:|
| Fast-ACO | 3.179 ms（线 +5.7%） | 11.813 ms（线 +3.7%） |
| Fast-LLPC | 3.065 ms（贴线 ✓） | 11.419 ms（贴线 ✓） |

LLPC 贴线 → 测量条件有效。1080p 配对 +3.56% 与单实例 +5.7% 的差异来自双上下文驻留模式（配对跑两侧同慢）。

## 6. 交付清单与后续

**保留**：`XS_TRIM=1`（128 家族两核 `_lt`）+ `engine.cpp` U8-all（units 修复 + Stage 3 尾段预置）。
**回退**：`LT_UP8` 三核（`#error` 闸）、e4m3 shared 别名、`LdsE`。
**跳过**：`LT_MRG8`（证据 §3）。

Stage 3 纯 FP8 方案的直接输入：§2.2 取证链（bias 朝向陷阱、FConvert/shared-e4m3 为坑、元素级构造 + ROW/直读为可用算子）、§2.3 的"fp8 WMMA 实测慢"结论（必须先解决再扩面）、§4 的尾段已就位。

## 7. 复现

```bash
bash asi/build/build_asi.sh build/asi-015-aco          # 最终配置
bash asi/build/build_runtime_trace.sh                   # runtime（默认 base）
# 门禁/测速口径同 §5；XS_TRIM-only 回归对照见 build/v015-runs/stage2/speed/{xst,up8f}-*
```
