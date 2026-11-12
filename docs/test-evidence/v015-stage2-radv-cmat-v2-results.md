# Stage 2 v2（勘误）：漏编 Bug 修复后的真工具链 + 128 家族 dont_unroll + Fast/Legacy 双模式扩面

本报告是 [`v015-stage2-radv-cmat-results.md`](v015-stage2-radv-cmat-results.md) 的勘误与接续。上一轮（`80fd0d7`）的全部三档测量实际运行在**未包含本轮改动的旧 `libvulkan_radeon.so` 上**——详见 §1。本文数据全部来自修复后的真工具链（`radv_sha256 17cc90d8…`）。

## 1. `80fd0d7` 漏编 Bug：根因、修复、哈希对照

**根因**：`build_capture_263.sh:28-34` 用"标记存在即跳过"守卫（`grep -q onr_capture` / `grep -q ONR_ACO_CMAT_ADDRESS`）。WSL 源码树早在 `aac9e52` 就带有这两个旧标记，于是 `80fd0d7` 修改的 `prepare_capture.py`（`ONR_ACO_SHARED_CMAT`）与 `prepare_global_address.py`（`elem_bytes`）**被静默跳过，从未落地**。

**修复**：`build_capture_263.sh` 改为无条件流程——从 tarball 还原 5 个受补丁文件原态 → 顺序执行两个 prepare 脚本 → 硬断言 `grep -q ONR_ACO_SHARED_CMAT` 与 `grep -q elem_bytes` 命中，否则 `exit 1` → 重跑 ninja → 刷新 `toolchain.json`。

| 指纹 | Bug 前（`80fd0d7` 实际所用） | 修复后（本文全部数据） |
|---|---|---|
| `capture.patch` | `27628e5a…`（旧，无 shared-cmat） | **`e266ee1b…`** |
| `global-address.patch` | `34c09029…`（旧，无 elem_bytes） | **`ed334844…`** |
| `radv_sha256` | `7fc443f3…` | **`17cc90d8…`** |

## 2. 真档位 I' / II' / III' 重筛（Fast，全部带真工具链）

### I'（`elem_bytes=1` FP8 通道 + 21 核清单）——**0/94 记录变化**
真工具链下 FP8QV 生产路径**根本不走 `cooperativeMatrixLoad`**：`onr_block_fast.glsl:689` 的 `XIN8` 用普通 u32 装载 + `uintBitsToFloate4m3EXT` 位解包；WSTEP 权重是 ROW-major；唯一命中 guard（COL/length8/mul1/global/stride∈{1,2}）的只有 fp16 负载。`elem_bytes` 通用化是**面向未来着色器的正确性基建**，对现有 94 核为无操作。**结论：`80fd0d7` 报告的 I 档 −2.80%/−1.63% 数据有效**（其 fp16 路径与真 I' 逐位相同）。

### II'（+ `ONR_ACO_SHARED_CMAT=load`）——**爆，判死**
真生效（3 核 `code_hash` 变化），但全部是灾难：

| 核 | exec 变化 |
|---|---:|
| `dec_dfn_96_128_f_lt` | **+13,236 B** |
| `dec_dfn_96_128_fl_lt` | **+13,288 B** |
| `enc_stage_96_128_128_f_lt` | **+13,092 B** |

补 `MLP_SLICE_LOOP [[dont_unroll]]` 到 `dec_dfn_96`/`enc_stage_96` **无效**（+12~13KB 仍在）——爆炸源不是 MLP 循环而是别的展开区域。

### III'（`loadstore`）——同构必爆，未跑（II' 已证伪该方向）。

**择优结论：最终配置 = 128 家族 `MLP_SLICE_LOOP [[dont_unroll]]` + 双模式 50 核清单 + shared-cmat off。**

## 3. 128 家族 `MLP_SLICE_LOOP` 守卫（`#if defined(ENC_F32) && ENC_F32`）

三处 `.comp`（`enc_stage_128_128_160`、`dec_dfn_128_128`、`dec_up_128_128_160`）在 `#include "onr_block.glsl"` 前定义 `MLP_SLICE_LOOP [[dont_unroll]]`，守卫覆盖 `_f`/`_fl`/`_f_lt`/`_fl_lt`，不动 faithful（`ENC_F32==0`）。

- **守卫本身零副作用**：11 个变体 spv 变（DontUnroll 标志）而 ACO 代码逐位不变（cmat-address 关闭时）。
- **守卫 + cmat-address**：128 家族 11 变体全部转正——`exec −888 ~ −2176 B`、零 scratch、零 VGPR 增。

**Legacy 铁律门禁（spv 已变，第一时间实测）**：Legacy-ACO 与 Legacy-LLPC vs 冻结基线（`3b23cef`/v0.1.2）steady+DRS 均 **exact 100.0000% / ulp 0** → 广义守卫保留。

## 4. 双模式 50 核清单静态表（vs `80fd0d7` 基线）

31 记录变化，全部为胜（`dec_dfn_64_64_f_lt` +540 为按计划退出清单的回摆；`enc_stage_64_64_96_f` +60 噪声级）：

| 家族 | 变体 | exec 变化 |
|---|---|---|
| 128（新入） | ×11（_f/_fl/_f_lt/_fl_lt ×3 核） | **−888 ~ −2176** |
| 160（_f/_fl 新入，_lt 已有） | ×8 | −1304 ~ −2520 |
| 96（_f/_fl 新入） | ×6 | −236 ~ −1556 |
| W1/W2（_f 新入） | enc0_stage ×2、dec_head、dec_up_32、enc_stage_64 | −60 ~ −1668 |

清单修正：`80fd0d7` 报告称剔除 `dec_dfn_64_64` 但脚本未剔——本轮已真剔除（静态 +540 回摆即证据）。

## 5. 双模式速度（3×48/240/64 ABBA vs 同版 LLPC）

### Fast（`--fast-mode 1`）

| 档 | ACO strict | Fast-LLPC | 差距 | `3b23cef` 时 | `80fd0d7` 时 |
|---|---:|---:|---:|---:|---:|
| 1080p | 3.197 ms | 3.038 ms | **+5.23%** | +11.61% | +7.49%* |
| 1440p | 5.497 ms | 5.278 ms | **+4.14%** | +10.08% | +6.55%* |
| 4K | 11.877 ms | 11.429 ms | **+3.92%** | +8.29% | +5.33%* |

\* `80fd0d7` 列 = 假工具链下测得（真 I' 与之逐位相同，数字本身有效，但当时的 II/III 对比无效）。

### Legacy（`--fast-mode 0`，`_fl` 深层核首次纳入 cmat-address）

| 档 | Legacy-ACO | Legacy-LLPC | 差距 | 扩面前 |
|---|---:|---:|---:|---:|
| 1080p | 3.588 ms | 3.440 ms | **+4.32%** | ~+12% |
| 4K | 13.509 ms | 12.977 ms | **+4.10%** | ~+10% |

### 单实例独立毫秒数（无 `--baseline-runtime`，对照门禁线）

| 配置 | 1080p | 门禁线 | 4K | 门禁线 |
|---|---:|---:|---:|---:|
| Fast-ACO | 3.177 | 3.009 | 11.955 | 11.389 |
| Fast-LLPC | 3.001 | 3.009 ✓ | 11.319 | 11.389 ✓ |
| Legacy-ACO | 3.617 | 3.468 | 13.515 | 13.123 |
| Legacy-LLPC | 3.458 | 3.468 ✓ | 13.027 | 13.123 ✓ |

LLPC 四组全贴线 → 测量条件有效。ACO 剩余差距：Fast +4.3~+5.0%、Legacy +3.0~+4.3%。

## 6. Step 3：蓝噪声种子 / 窗口奇偶相位对齐

**修复**：`openrr_contrast_eval.cpp` 预热循环 READY 后、正式 f000 前补相位对齐循环——继续提交 priming 帧直到 `info.frames % 8 == 0`（同时钉住 `frame_ & 1 == 0`），超时保护 120s、最多补 7 帧。

**v1 对齐前后（per-case 均值 PSNR，01-norm dB，越高越好）**：

| 用例 | legacy | fast-llpc | fast-aco |
|---|---:|---:|---:|
| Q_S0_1080p_dlaa | 44.70→**45.99** (+1.29) | 46.55→**47.76** (+1.21) | 46.43→**47.72** (+1.29) |
| Q_S0_1080p_qual | 42.29→**44.07** (+1.78) | 43.14→**45.10** (+1.95) | 43.10→**45.10** (+2.00) |
| Q_S0_1440p_qual | 41.70→**43.31** (+1.62) | 42.19→**43.95** (+1.75) | 42.20→**43.94** (+1.74) |
| Q_S0_4k_perf | 48.15→**49.53** (+1.38) | 48.23→**49.55** (+1.32) | 48.22→**49.55** (+1.33) |
| Q_S1_1080p_qual | 43.01→**44.85** (+1.84) | 42.71→**44.38** (+1.67) | 42.72→**44.39** (+1.68) |
| Q_S2_4k_perf | 39.94→**40.85** (+0.91) | 39.50→**40.25** (+0.75) | 39.52→**40.23** (+0.72) |

**v1 Q_S0_1080p_dlaa f063 分叉**：LLPC−ACO gap 0.76 dB → **0.26 dB**（其余帧 gap ≈ 0.02 dB 量级；f063 处残差是 DRS 边界帧的固有抖动，不再是系统性错拍）。

**v2_motion 对齐前后（核心切片）**：

| 用例 | legacy | fast-llpc | fast-aco |
|---|---:|---:|---:|
| Q_S1_1080p_qual | 45.81→**47.44** (+1.62) | 45.04→**46.39** (+1.35) | 45.04→**46.39** (+1.35) |
| Q_S1_4k_qual | 46.93→**48.32** (+1.39) | 46.74→**48.04** (+1.30) | 46.74→**48.05** (+1.31) |
| Q_S2_1080p_qual | 44.11→**45.32** (+1.21) | 41.08→**41.60** (+0.52) | 41.06→**41.60** (+0.54) |
| Q_S2_4k_perf | 45.09→**46.40** (+1.31) | 43.34→**44.03** (+0.69) | 43.33→**44.03** (+0.70) |
| P00_baseline | 46.95→**48.31** (+1.36) | 45.95→**46.96** (+1.01) | 45.99→**46.98** (+0.99) |

全部用例 `non_finite == 0`、参考帧 SHA 门禁全绿（v1×3 档、v2×3 档）。对齐后 fast-llpc 与 fast-aco 差 ≤0.02 dB（数值上已收敛）。CSV 归档：`docs/test-evidence/anchor2-phase-aligned/{v1,v2}/`。

## 7. 门禁汇总

| 门禁 | 结果 |
|---|---|
| `test_asi_units` | ALL PASSED |
| Legacy-ACO vs 冻结基线（steady+DRS） | exact 100.0000% / ulp 0 |
| Legacy-LLPC vs 冻结基线（steady+DRS） | exact 100.0000% / ulp 0 |
| Fast-ACO vs 父构建（steady+DRS） | exact 100.0000% / ulp 0 |

## 8. 后续课题

1. 96 家族 shared 向量化：爆炸源非 MLP 循环；候选修法 = 定位真实爆炸循环打 `dont_unroll`、补丁调 `nir_opt_loop_unroll` 预算、或向量宽度 8→4。
2. `elem_bytes=1` FP8 通道：待未来着色器把 FP8QV 装载改为 COL-major coopmat load 后自动生效。
3. 剩余 +4% 差距的主体是 128 KiB/WGP LDS 物理上限封顶的 waves（§7 勘误），需 tile 布局手术。

## 9. 复现

```bash
bash asi/build/build_asi.sh build/asi-015-aco        # 最终配置（50 核 + 128 守卫 + shared off）
OPENRR_ACO_SHARED_CMAT=load bash asi/build/build_asi.sh build/asi-015-aco   # II'（会爆炸，仅复现用）
# 测速：build/v015-runs/stage2/final_speed.ps1（-FastMode 0/1，-BaseComp llpc）
# 画质：build/aco-runtime-trace/openrr_contrast_eval.exe（相位对齐已内置）
```
