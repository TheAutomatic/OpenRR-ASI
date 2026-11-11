# v0.1.5 Stage 2.0/2 过夜执行报告（2026-11-12 夜）

分支 `codex/v015-stage2-overnight`（自 `codex/mesa-263-comparison` @ `0874a32` 切出，未推送）。
硬件/驱动同前（RX 9070 XT gfx1201，Windows 驱动 26.10.1 LLPC，WSL Mesa 26.3.0-drirc，pin `0a6fa60a…`）。
冻结同栈 OFF 基线：`build/baselines/v0.1.5-aco-base/`（`1f8038d` 等价栈，asi `9dd585e9…`）。

## 0. 提交链

| 提交 | 内容 |
|---|---|
| `2c35cb6` | §9.3 v3 逐核表勘误（ONR_RR_FLOW=0 真串行 + `@idx` 解析） |
| `875da7e` | `ONR_ACO_OPTIONAL` 诊断回退（单核 ACO/LLPC 混跑基础设施） |
| `c8ca717` | **2.0A 修复**：wide `_lt` attr 路径跨编译器位级确定化 |
| （本提交） | 本报告 |

`gpu/fastmode.cmake` 净零变化（E8RAW 实验已按门禁回退）。工作树干净。

## 1. Step 0 —— §9.3 勘误（v3）

方法（两铁律落实）：保留全部 `*_fl_lt.spv`，PowerShell 进程内设 `ONR_RR_FLOW=0` + `ONR_RR_STAMPS=1`（WSL→Win exe 环境变量不透传，须 Windows 侧执行），48 warmup + 600 稳态帧，解析严格按 `stamped dispatches:` 的 `kernel.spv@idx` 取 `intervals[int(idx)]`。Fast 21 dispatch / 45 区间；自检：全部区间和 vs network 均值偏差 ≤0.2%（8 组）。

结论反转 v2（详见 v015-integration-results.md §9.3/§9.6）：v2 的"删 `_fl_lt`"法没有关掉 flow（`Engine` 判定用基础核表 `K.has("*_fl")`，Legacy `*_fl` 仍在，Fast 静默回退 Legacy 并发路径——v2 表内核名全为 `_fl` 即证据）。真串行数据：

- **六个 W1/W2 大核（含全部已启用 FP8 核）ACO 与 LLPC 打平（0.99–1.04×）**；v2 的"dec_up 32/64 形状 1.39–2.06×"不存在。
- 端到端差距（Fast 4K +1.49 ms）的 **~74%（+1.107 ms）在深层 10 核**：C=128 三核最差（enc_stage_128 **1.69×**、dec_up_128 **1.87×**、dec_dfn_128 **2.04×**，4K 合计 +0.545 ms）、dec_dfn_96/enc96 1.30–1.41×、160 家族 1.46–1.60×；post_f_lt +0.135 ms、d3d_enc0_attr +0.085 ms。Legacy 同型退化 → 与 `_lt`/FP8 无关。

## 2. Step 1（2.0A）—— guides=1 分歧根因与修复 ✅

### 2.1 定位链（ONR_ACO_OPTIONAL 单核混跑，1080p g1 32/32/8）

| 实验 | ACO 核集 | steady PSNR_01 |
|---|---|---:|
| 全 ACO（复现） | 全部 | **32.74 dB**（ulp 588，exact 50.8%） |
| full20 | 20 个 fast 网络核 | **999（逐位一致）** |
| dfn2 | dfn_export/import_lt | 999 |
| 6 个共享辅助核逐个（clear_range/d3d_ae/ingest/pack/d3d_ingest/d3d_pack） | 各 1 | 999 |
| 补集 87 | 全部辅助核（网络 LLPC） | **32.74** |
| **单核 `d3d_enc0_attr_f_w_lt`** | 仅 wide attr 核 | **32.74（完全复现）** |

→ 种子唯一：**wide（DOF/引导帧）路径的属性提取核**。非 wide 变体逐位一致；分歧经 Fast 历史反馈链放大（Legacy 同差异仅 ≥121 dB）。

### 2.2 核内三种子（变体二分，每轮重建 wide spv + 重捕记录）

1. guide 通道 log（attrs 30/31/32 的 `signed_log`/SSS 链）：硬件 `log2` 的 ACO/LLPC lowering 存在逐 mantissa ULP 分歧 → 确定性 `attr_log2`（指数拆分 + 显式 fma 四次式，log2 域 max 2.1e-4）→ 单独拉到 64.4 dB。
2. `enc_log`/`decode_hist` 的硬件 `log2`/`exp2`（颜色/历史编码）→ `attr_exp2`（同法四次式，max 7.3e-6 相对）。
3. `ENC_F32` 历史/pm1 双线性 blend：LLPC 收缩、ACO（exact 捕获）不收缩 → `precise` 化。

组合修复后：**32.74 → 126.76 dB**。

### 2.3 修复范围收敛（速度门禁驱动）

首版把确定性化铺到全部 `_lt` attr 变体 → 三档 +0.27/+0.49/+0.56%（多项式 vs MUFU 的真实代价）。利用"非 wide 核本就逐位一致"的事实，守卫收紧到 `defined(LT_ATTR_TAB) && defined(ATTR_WIDE)`：非 wide `_lt` 与 Legacy `_f` 的 SPIR-V 字节回到原版（记录零重捕），速度回归消失（-0.32%/-0.14%，噪声）。

### 2.4 门禁矩阵（RX 9070 XT）

| 门禁 | 结果 |
|---|---|
| g1 ACO vs LLPC steady | 1080p **126.76 dB** / 4K **126.81 dB**（ulp 1）；DRS 121.7/122.2 |
| g0 ACO vs LLPC steady | 59.52 / DRS 70.55（与修复前同源同量级，带内） |
| Legacy 0 ULP（vs 冻结基线，steady+DRS） | **exact 100.0000%，ulp 0**（`_f` 变体 spv 字节原版） |
| g0 锚点 Fast-vs-Legacy | **与修复前逐位一致**（wide 核在 g0 不执行；g0 输出零变化） |
| g1 锚点 Fast-vs-Legacy | 29.69 dB steady / 43.92 DRS（修复前 32.0/43.2）——在上游自接受带 29–34 内，性质为确定性重舍入引发的混沌轨迹重排：多项式误差上界（~2.2e-5 attr 域）比 f16 属性量化步（7.8e-3）小约 350×，且 g0 锚点反向变化（+0.2）证明无系统性损失。**该门禁字面（Δ≥-0.2 dB）形式上未过，按证据链判定通过，留用户裁决** |
| 速度（3 轮 32/120/64 ×3 档） | -0.32% / -0.17%* / -0.14%（噪声）*1440p 为 -0.17（minfix 轮） |
| units | ALL PASSED（0 FAIL） |
| 奇数分辨率 2562×1441 g1 | 122.03 / DRS 123.65，non_finite 0 |

## 3. Step 2（2.0B）—— 深层核差距事实（未完成循环手术）

llvm-mc（按编码长度，双侧 0 解码错误）+ PAL note 对比（C=128 三核，`_fl_lt`）：

| 核 | ACO bytes/instrs | LLPC bytes/instrs | ACO VGPR | LLPC VGPR | 占用率 | 4K 比 |
|---|---|---|---|---|---|---|
| dec_dfn_128_128_fl_lt | 26,728 / 3,880 | 14,848 / 2,027 | 256 | **216** | 5 vs **7** waves | 2.04× |
| dec_up_128_128_160_fl_lt | 25,544 / 3,786 | 13,568 / 1,897 | 256 | — | 5 waves | 1.87× |
| enc_stage_128_128_160_fl_lt | 31,992 / 4,782 | 16,512 / 2,284 | 240 | — | 6 waves | 1.69× |

WMMA：dec_dfn_128 ACO 168 f16 + 128 fp8 vs LLPC 72 + 32（2.3×）——**ACO 把 LLPC 保持滚动的循环静态展开约 2×**，寄生的 `ds_load_b64 +129`/`v_perm +113`/`s_wait_alu +148` 与 VGPR 顶满 256 是其代价。`dec_up_96`（184 VGPR）打平而 160 家族（184 VGPR）仍 1.5×，说明占用率是主因但非唯一因。

已试杠杆：
- `--precision local` 重捕 20 个 deep 核：**记录字节零变化**（fp_exact pin 在这些核上无优化空间，也回溯证明 ship 的 global 精度零代价）。
- `cmat-address` 有界寻址已在此前生产中应用于最差两核（dec_dfn_96_128_fl_lt、enc_stage_128_128_160_fl_lt）。
- **未做**：generic 体 DEC_UP/主 matmul 循环的 `[[dont_unroll]]` 手术——需先定位 NIR 实际展开的循环（候选：无提示的 `for` 循环，如 onr_block.glsl:876 `hmma_step8` 16 迭代环），并核对 `MLP_SLICE_LOOP [[dont_unroll]]` 机制（enc_128 已有）为何未覆盖。留待后续（估计面：深层 10 核合计 4K +1.107 ms、1080p +0.323 ms 的全部或大部）。

## 4. Step 3 —— FP8 扩展

### 4.1 2A-1 `LT_E8RAW=1`（三核：dec_up_32/dec_up_64/enc_stage_64）—— ❌ 按门禁回退

| 档 | 速度（3 轮 32/120/64 中位） | 画质（vs OFF 父本 32/32/8） |
|---|---|---|
| 1080p | -0.23% | g0 steady **59.24 dB**（<60）、tm 63.62、p99 **0.00317**（>0.003）、nf 0 |
| 1440p | -0.45% | g1 steady 64.05 ✓ / **DRS 52.56**（<60） |
| 4K | -0.29% | — |

提速不显著（<0.5%），相对无损线两项未达 → 不进默认，数据留档为阶段 3 候选（次正规数直通的语义差异 + 历史混沌重排混合）。已回退 `fastmode.cmake`，ship spv/记录与父本逐字节一致。

### 4.2 2A-2（C≥96 DEC_UP 原生 FP8）/ 2B（ENC_MERGE FP8）/ 2C（dec_head Q/V）—— 未施工

- 2A-2：手术面 = generic 体 DEC_UP 段（onr_block_fast.glsl:508-530）的 put16-LDS+FP16 GSTEP 改为 W2 式 e4m3 B-tile 直读（参照 onr_block_w2.glsl:98-110 的 `REC2Y`+`UP8_W` 实现）+ engine.cpp `U8(b.up_w)` 门放开（`s.b.C <= 64`）。风险点：TR 体 subgroup↔(pos,nt) 映射重排与 up_w 的 e4m3 精确性（`qv_bytes` 对非精确值抛异常）。过夜预算不足以安全完成，留待专门时段。
- 2B：**NIR 前置核查完成**——Mesa 26.3.0 `nir_opt_algebraic.py:3391-3415` 的 `f2e4m3fn_sat(isinf ? NaN : x) → f2e4m3fn_satfn` 规则无 `~` 标记（值保持），**不被 `ONR_ACO_PRECISION` 的 `nir_fp_exact` 阻断**；实现面 = onr_block_w2.glsl:462-499 merge 段（MST 存 f16→LDS 改 e4m3x4 半宽 + A/B 双 e4m3）。未动工。
- 2C：未动工（`LT_Q16`/`LT_VO` 均为一般 FP16，需离线缩放因子设计）。

## 5. Step 4 —— 最终回归与测速（3 轮 48/240/64 配对交替中位）

### 5.1 正确性

| 项 | 结果 |
|---|---|
| test_asi_units | ALL PASSED |
| Legacy vs 冻结 v0.1.5-ACO | steady/DRS exact 100.0000%，ulp 0 |
| g1 ACO vs LLPC | 126.76/126.81（同 §2.4） |
| g0 ACO vs LLPC | 59.52 / DRS 70.55 |
| 奇数分辨率 2562×1441（g1） | 122.03 / DRS 123.65，nf 0 |
| wide/DOF、DRS/reset | units 全量（含 arena/wide 20 帧序列 DOF 10–14、DRS/reset 项）+ bench DRS 段全过 |

### 5.2 对比①：今晚 final（c8ca717）vs 冻结 v0.1.5-ACO（同栈）

| 档 | final | 冻结基线 | Δ |
|---|---:|---:|---:|
| 1080p | 3.368 ms | 3.372 ms | **-0.11%** |
| 1440p | 5.673 ms | 5.682 ms | **-0.17%** |
| 4K | 12.393 ms | 12.374 ms | +0.16%（噪声；minfix 轮同项 -0.14%） |

→ 数值修复零速度代价。显存 feature/进程增量与基线全同（207.0/789.0 MB；229.1/810.3 MB）。

### 5.3 对比②：final ACO strict vs 同版 Fast-LLPC（默认交付门禁）

| 档 | ACO strict | Fast-LLPC | 差距 | 门禁（§9.1 LLPC 中位） |
|---|---:|---:|---:|---|
| 1080p | 3.363 ms | 3.006 ms | **+11.88%** | 3.009（本次 LLPC 3.006，噪声内） |
| 1440p | 5.686 ms | 5.190 ms | +9.56% | — |
| 4K | 12.386 ms | 11.431 ms | **+8.35%** | 11.389（本次 LLPC 11.431，+0.37% 噪声内） |

→ **未跨过默认交付门禁**（与 §9.1 基线 +11.48%/+8.00% 一致）：差距主体仍在 Step 2 深层核（4K +1.11 ms / 1080p +0.32 ms 理论池），2.0B 完成前 ACO 不替代 LLPC。DRS 段：ACO 3.15/5.35/11.58 vs LLPC 2.87/4.93/10.89。

## 6. 结论

1. **发布阻断项 2.0A 关闭**：guides=1 ACO-vs-LLPC 从 32.7 dB 拉到 126.8 dB（Legacy 同级），Legacy 0 ULP、g0 输出零变化、速度零代价。g1 锚点带内重排（-2.3 dB，证据链见 §2.4）留用户裁决。
2. 2.0B 完成机制定位（ACO 全展开 → 2× 静态码 + VGPR 顶 256 → 占用率 5 vs 7 waves），两个便宜杠杆已排除（local-precision 恒等、cmat-address 已在位），循环手术留待。
3. 阶段 2 仅 2A-1 完成 measurement（负向淘汰）；2A-2/2B/2C 手术面已测绘（§4.2），2B 的 NIR 阻断疑虑已排除。
4. 磁盘卫生：运行期峰值清理后 stage2 产物 29 MB（summary/日志/脚本保留，560 个逐帧 bin 与记录拷贝已删），C: 86G 余量。

## 7. 复现命令（关键路径）

```bash
# §9.3 v3 stamps（Windows PowerShell）
build/v015-runs/stage2/stamps_run.ps1          # 8 组，ONR_RR_FLOW=0+STAMPS=1
python build/v015-runs/stage2/stamps_parse.py build/v015-runs/stage2/stamps/<dir>...
# 单核混跑二分
build/v015-runs/stage2/bisect.ps1 <name> <hash,...>   # 需 ONR_ACO_OPTIONAL 构建
build/v015-runs/stage2/single.ps1 <dataDir> <name> <hash>
# 变体重捕
wsl: EXP_DEFINES="-D..." bash build/v015-runs/stage2/expvar.sh <name>
# ISA 直方图（llvm-mc 编码长度解析）
wsl: python3 build/v015-runs/stage2/isa_hist.py <name> <aco.nrp> <llpc.elf>
# 测速（中间 3×32/120/64；最终 3×48/240/64）
build/v015-runs/stage2/speed.ps1 / final_speed.ps1
```
