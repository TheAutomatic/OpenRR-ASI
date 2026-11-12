# Stage 3：MLP 原生 FP8（`LT_MLP8`，交付）+ `hist_` RGB10A2 压缩（判负回退，双阻塞留档）

分支 `codex/mesa-263-comparison`（`7ca278a` 之上）。Step 0（构建除雷）见提交 `e6ca8fd`；Step 1 见 `50bc902`；本报告汇总 Step 1 全量证据与 Step 2 的负结果取证。

## 0. Step 0 摘要（`e6ca8fd`）

- `build_runtime_trace.sh` 补 `-DOPENRR_EXPERIMENTAL_OPTIFG_HOST`（`build_asi.sh:36` 同款——此前 base 对象 ABI 不配的根因）；`OPENRR_TRACE_BASE=build/asi-015-aco` 原生可用并自动同步 `build/aco-v015`（测速实际加载目录）——**陈旧 runtime 静默漏更的陷阱（`80fd0d7` 假档位的根因）自此结构性封死**。
- 清 `build/aco-runtime-trace/stage`（v0.1.4 spv 影子目录，会劫持 units 的 `exe_dir` 解析）；trace 侧冗余 units 副本删除（canonical units 是唯一门禁）。
- `v015-stage2-up8-mrg8-results.md` 勘误：UP8 v5 非"原生 FP8"（A/B 仍 fp16 装载 + 每 tile 32 次内层标量转换），其 −0.9pp 不能外推到真原生路径；`engine.cpp` 注释改为符合 wb blob 事实的表述。

## 1. Step 1：MLP `fc1`/`fc2` 原生 FP8 —— **交付（`50bc902`）**

### 实现
- **权重侧**（`engine.cpp`）：`e4m3_rne()`（fp16 位→OCP e4m3，RNE + satfinite ±448，NaN 码永不产生）；fc1/fc2 缓冲在 fp16+tiles 之后追加原始矩阵的 e4m3 连续 `[row][K]` 尾段（`UT8`）。无 per-tensor 缩放：权重远离 ±448 与次正规下限，2 的幂缩放不改变浮点格式的相对量化误差。Legacy 只读前缀。
- **着色器侧**（`onr_block_fast.glsl`，`LT_MLP8`）：权重按 1 字节 `QA8` ROW 直读尾段（`W8STEP` 同款，乘积循环内**零转换**）；激活在复用域外一次性量化（`qe4` = fp16 clamp + 构造器，Mochi `NR_QUANT` mode-1 语义，无 MODE 寄存器）——x1 tiles 在 slice 循环外量化一次（跨全部 `4·NT` 个 fc1 tile × 全部 slice 复用），GELU 对每 slice 量化一次（跨全部 `NT` 个 fc2 tile 复用）。fp8 WMMA 定 f32 累加：slice 累加器与 fc2 局部 tile 用 `QC8`，循环后一次折回块级 fp16 输出。
- **启用面**：仅 `dec_dfn_128_128` + `enc_stage_128_128_160`（`_f_lt`/`_fl_lt`）——96/160 深层核走 SPLIT2 分支（代码不达），`enc_stage_96` fl 变体跨 Mesa 展开成本上限（+11.5KB）剔除。静态：VGPR 全部稳住（192/240，零 scratch），exec +1.7~2.0KB。

### 门禁与测速
| 项 | 结果 |
|---|---|
| `test_asi_units` | ALL PASSED |
| Legacy vs 冻结基线（steady+DRS） | exact 100.0000% / 0 |
| 非 `_lt` SPIR-V 抽查 | 与基线逐字节一致 |
| 内部画质 vs 父 | **59.21 dB**（ulp 75，双向 e4m3 量化） |
| **锚点 #2**（相位对齐 v2_motion vs RTX 5090） | Q_S1 −0.005/−0.006 dB，Q_S2 −0.033/−0.005，P00 −0.012；`non_finite=0`，哈希门禁全绿——远低于 0.5 dB 准入线 |
| 速度 ABBA vs 父 | −4.60% @1080p / **−3.82% @4K**（XS_TRIM 单独为 −4.70/−3.52：4K 净 +0.30pp，1080p 噪声带） |
| 终局配对 vs 同提交 LLPC | +4.50% @1080p / +3.45% @4K |
| 单实例 | Fast-ACO 3.171 / 11.848 ms（LLPC 3.032 / 11.376，双贴线） |

## 2. Step 2：`hist_` RGB10A2 仿射压缩 —— **判负回退（未提交实现，取证留档）**

### 已完成并验证的部分
- 数学与格式设计成立：log 域 `v ∈ [−1.44, 3.0+]`，读侧 `post_decode` 本就 clamp 到 `[1e-4, 3.0]` → 仿射区间 `[−1.5, 3.0]`（span 4.5）**零语义损失**；仿射与硬件/手工双线性严格交换；alpha 通道全死（post 写 0，所有读者只取 .xyz）——2-bit 白送。
- 全部读写点改齐（post 读写、downsample_tl 的 DRS 手工双线性读写、b2/occ 探针变体）后编译全绿；`srd_ok=1` 的 bench 上下文里 **RGB10A2 linear alias 本身成功**（vkCreateImage + rowPitch + 绑定全过）。

### 两个硬阻塞（架构级，非 bug）
1. **`fast_` 不是 `_lt` 旗标**：Legacy 模式下引擎同样 `fast_ = true`（engine.cpp:583——fast 指 F32 构建，Legacy/Fast 之分在 lite 梯子）。以 `fast_` 门控缓冲格式会让 **Legacy 也拿到 RGB10A2 缓冲而其 fp16 着色器照读 8 字节跨距** → 秒崩。正确旗标是 `frame_lite_`（`lite_on_ && lk_ && fast_`）。
2. **双 post 架构要求两种格式同时在场**：`lite_post`（engine.cpp:868）是**每帧条件**（reset 位 16 + `render×3 ≤ internal×2`），不满足时 Fast 模式同样跑**非 lite post**（`hist_texel` 的 fp16 8 字节 buffer 读写）。缓冲格式是分配期决策，无法跟每帧条件切换——除非双缓冲（显存翻倍，背弃 4B/px 初衷）或统一 post 路径（深手术）。

### 结论与重开条件
按"负向当场回退留档"协议，全部改动还原至 `50bc902`；回退后 units ALL PASSED、Legacy 100%/0、Fast 59.21 dB 特征值复现 ✓。**重开前置条件**（任一）：
- post 单路径化（`lite_post` 条件消除或非 lite post 同步 HIST10 化并接受 Legacy 数值变化——需重新过 0-ULP 锚点）；
- 或 per-frame 双 hist 缓冲 + 显存预算重审（4K 预期收益 149.3 MB 会被第二个缓冲吃回 ~75 MB）。

预期收益（未测得，供重开时参考）：4K `hist_` 双缓冲 789→640 MB 显存、post/attr 历史带宽减半、`post_f_lt`/`d3d_enc0_attr_f_lt`/`d3d_downsample_lt` 提速。

## 3. 复现

```bash
bash asi/build/build_asi.sh build/asi-015-aco          # 最终配置（含 LT_MLP8）
OPENRR_TRACE_BASE=build/asi-015-aco bash asi/build/build_runtime_trace.sh   # runtime 自动同步 aco-v015
# 锚点 #2 归档：docs/test-evidence/anchor2-phase-aligned/v2/v2-fast-aco-mlp8-per-frame.csv
```
