# Stage 8 Phase 1: v0.1.6 合流、Fast=2 入口、三对照基线与 C=160 WGP 单测（2026-11-16 夜）

分支 `codex/mesa-263-comparison`。锚点：`stage7-b2-15286e3`（tag，代码态 `6dc4c99`，工作树包 `build/v015-runs/stage8/ref-stage7-b2/`，构建于 11-15 19:52 = 6dc4c99 后、docs-only 提交前）。

## 1. 合流（merge `5dd5b0f` + 适配 `5091c02` + 日志修复 `070995f`）

- `git merge asi-plugin`（`7e1df92`，上游 v0.1.6 `3629dab` + 8 个兼容提交），7 处内容冲突全部手工解决。原则：**Stage 5–7 晋级 FP8 路径全部保留**（`LT_MLP8`/`LT_EMB8`/`LT_MRG8`/`FP8QV` 量化一次 XOUTQ、MRG8 LDS 代码镜像、`W2_FC1Q` e4m3 fc1 尾段），v0.1.6 的 RDNA3 布局桥（`onr_frag.glsl` FRAG_ROWS、`XPB`/`XROWS8`/`EMB_X`/`YST`）进入所有 fp16 回退路径，NGX `tex()` 绑定链取上游参考对齐版（b456ee0），`LT_DS_TLLDS` 与 `opt_r32` 视图直读吸收。
- 合流冒烟：12 个晋级 `_lt` 核 + 7 个 gfx11 变体 glslang 全过。
- 四项适配：
  1. **gfx11 纯 FP16 剥离**（`build_shaders.py` + `gpu/CMakeLists.txt`）：strip 集扩至 `LT_MLP8/LT_MRG8/LT_EMB8/LT_QV8/LT_DFN8/LT_E8RAW/FP8QV`；94 spv 全过、**零 e4m3 残留**（扩展名串扫描 0/94）。首跑翻车根因：`-DFP8QV=0`（全局）撞 `LT_DEFS … FP8QV=1`，已入 strip 集。
  2. **Guide NaN 位级判定**（`onr_guides.gtex`、`enc0_attr.opt_r32`）：`isnan(x)` → `(floatBitsToUint(x) & 0x7fffffffu) > 0x7f800000u`，防 `--precision-native-only` 折叠 NaN→0 清洗。
  3. **探针清零**：`build_asi.sh` 删除 `occ_nopad/occ_pad16k/b2_*` 生成与 ACO records 输入，manifest 恰 94 条官方记录；启动自清理防陈旧探针复活 K2 门（旧包残留曾使 units 在 `post_f_lt` 旧哈希上 FAIL，清理后 27 项全绿）。
  4. **`aco/gfx1201/openrr-d3d12-v1/` 随 stage 打包**（`--out "$stage/openrr-amd/aco"` 原有布局确认）。
- 静态日志路由修复（`070995f`）：静态构建宿主指定日志路径（bench 按运行目录契约）恢复直写 + 全量镜像 `rr_pe_log` 崩溃观察trail；模块目录默认日志仍归 pe::log。首跑候选侧 per-run 日志被吞即此因。

## 2. Fast=2 入口（前置必做）

- `openrr_bench`：`--fast-mode 2` 就绪 = 状态 **4**；等待环外新增守卫——状态 1（`_lt` 静默回退）即报错退出。
- `openrr_contrast_eval`：新增 `--config exp-aco`（fastMode=2, compiler=0）/ `exp-llpc`（fastMode=2, compiler=1），就绪判定 `cfg.fastMode == 2 ? 4 : 1`。
- 实载验证（`fast2-probe`，1080p 48w+64f）：`enc0_stage_32_64_f_lx`、`dec_up_32_64_64_f_lx`、`dec_head_32_64_f_lx`（+ `_w_lx`）全部 `compiler=ACO` 命中 `.nrp`，返回状态 4，rc=0。

## 3. 三对照 + 双列 Anchor #2 + A/A 噪声带（测量统一 `ONR_RR_FLOW=1`）

**测量口径修正**：v0.1.6 宿主默认 `barriers (D3D12 default)`（`cc45be8` 崩溃防护）；所有基准/画质运行显式 `ONR_RR_FLOW=1`（源码注释即测量开关），与固定 `4332470` 账本口径一致。

| 对照 | 内容 | 结果 |
|---|---|---|
| ① `ref-stage7-b2`（15286e3 冻结包） | 22 核 @idx 4K + 3×ABBA 双分辨率 | @idx：94 记录中 81 逐字节一致；13 核静态变化全部可解释（enc0_attr 家族 = NaN 位级+视图直读；downsample_tl = TLLDS；enc0/dec_head `_f` = w1 布局桥代码重排，VGPR 216→240）；**帧级 ABBA：4K −0.028%（4/6 负，噪声内），1080p +0.411%（0/6 负，噪声外，≈+12µs）** |
| ② `ref-016-release`（7e1df92 原生包，独立 worktree 构建，LLPC） | **Anchor #1**：Legacy=0 位级 | **1080p & 4K steady+DRS 全部 100.0000% 0-ULP（999.00 dB），parity_passed=True** |
| ③ `ref-016-samehost-llpc`（同宿主同 SPIR-V，仅 `--candidate-compiler llpc`） | @idx 4K + 3×ABBA 双分辨率 | **LLPC 帧级慢 +18.85% @4K（0/6）/ +17.07% @1080p（0/6）**；核级 +18.7%（FP8 核最重：enc_64 +49.0%、dec_up_96 +50.0%、dec_head +26.7%；LLPC 反超仅 downsample −10.2%、enc0_stage −9.8%、dec_up_128 −17.5%、enc0_attr −6.8%）。旧跨包 LLPC 差值全部作废 |
| A/A 噪声带（同包自比 3×ABBA） | 4K / 1080p | 中位 +0.014% / −0.090%；对带 ±0.23% / ±0.26%。**未改动核 ±0.5% 为异常排查触发线** |

**双列 Anchor #2（fast-aco，v1 76 + v2_motion 44）**：合流包哈希门 v1 76/76、v2 44/44 全过，双侧 non_finite=0；与合流前 `final-anchor2` 逐帧 PSNR 差 **全部 +0.0000 dB**（画质零变化，`Q_S0_4k_perf` 52.0368 / `Q_S1_1080p_qual` 39.0973 与晋级时一致）。相对固定 `4332470` 累计账本不变；相对 `frozen-016-base` 本轮增量差 = 0（首个封板点）。

**水位封存（New=1，ACO，`frozen-016-base`）**：4K（500w+600f）**11.0347 ms**、1080p（1500w+600f）**2.94604 ms**。RTX 5090 同口径比值：**1080p 3.38x / 4K 3.79x**。Fast=2 优化前封存：4K **10.8906 ms** / 1080p **2.91524 ms**（较 New=1 仅 −0.5%/−0.7%，Phase 4 主战场）。

**遗留观察**：@idx 口径下 `enc0_stage_32_64_f_lt` 稳定 +43µs（两次复测 +4.85%/+4.88%）但记录逐字节一致且 4K 帧 ABBA 平——判为 stamps 模式与上游(attr/ae 代码变化)的交互伪影，Phase 3 同包协议复核。

## 4. C=160 WGP vs CU 单测

@idx 4K（vs ref-stage7-b2 CU）：`dec_dfn_160_fl` **+13.2µs（+13.1%）**、`enc_stage_160_256_0_fl` **+17.7µs（+15.7%）**、`enc_stage_160_256_160_fl` +2.6µs（+1.1%）、`dec_up_160_fl` −1.6µs（−4.0%）；全核合计 +54.9µs（+0.52%）。3×ABBA 帧级：**+0.267%（中位 +29.4µs，0/6 负）**，出噪声带。**结论：晋级 FP8 栈保持 CU 模式**（v0.1.6 的 WGP 声明基于未晋级核），归档判负，翻案条件 = 160 族 LDS/驻留重设计使 WG 尺寸效应反转。


## 5. 工具与产物

`build/v015-runs/stage8/`：`run_bench.ps1`（idx/steady/abba，ONR_RR_FLOW=1 钉死）、`run_llpc.ps1`（对照③）、`run_anchor1.ps1`、`run_anchor2.ps1`（支持 exp-aco/exp-llpc）、`idx_report.py`、`make_wgp_records.py`、`freeze.sh`。冻结包：`ref-stage7-b2/`、`frozen-016-base/`（trace asi 已刷新为日志修复版）、`wgp-pkg/`。运行目录均一次性新建、退出码 0 + 产物新鲜度校验（IDX-DONE/ABBA-DONE 门）。
