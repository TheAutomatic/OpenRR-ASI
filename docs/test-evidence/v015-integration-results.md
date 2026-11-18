# OpenRR v0.1.5 分层合入 + 26.3.0-drirc 全量重捕 + 新基线测量（2026-11-11）

上游 v0.1.5（tag `v0.1.5` = `d9a3b2ddabb125604f948ae9394b946c21b381be`）按既有分层合入：
`upstream/main → main（快进）→ asi-plugin（merge）→ ACO 分支（rebase 线性化）`。
捕获工具链不变：Mesa 26.3.0-drirc（release tag commit `0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4`），单工具链、global/vector8。

保护：tag `aco-014-prod` = 原 `992b6f8`，tag `asi-014` = 原 `9cfa9d7`；0.1.4 生产线产物冻结于
`Downloads/OpenRR/build/baselines/aco-014-prod/`（227 文件 SHA256SUMS，asi `6260e21c…`/bench `03f87b50…`/units `6d03a142…` 与交付记录一致）。

## 1. 分层合并

### 1.1 main
`fc61896`（v0.1.4）→ `d9a3b2d`（v0.1.5）纯快进（`git fetch upstream main:main`）。

### 1.2 asi-plugin merge（bdbbd0f + 两个后续修复）
提交 `merge: integrate upstream v0.1.5 while preserving ASI lifecycle fixes`。10 个冲突文件，取舍：

| 文件 | 取舍 |
|---|---|
| `gpu/fastmode.cmake` | 上游 post/post_dlt（LT_POST_HW/LDS/EXPO）整体采纳；B2 三宏（EXP/NOCORNER/NETH）退出 level-1，留待 §5 复核 |
| `gpu/include/opennr_rr/engine.hpp` | 并集：上游 set_deep/set_deep_on + asi fp8_ok 接口 |
| `gpu/src/engine.cpp`（resize） | asi 的 need_hist/batch/end_batch 结构 + 上游 deep_t_/deep_d_ 句柄复位 |
| `gpu/src/engine.cpp`（flow 清零） | 上游 deep 复用/迟到分支逻辑 + asi 的 10*1280*4 全清审计（上游三段式 fill 的总长同比例放大） |
| `gpu/shaders/downsample(_tl).comp` | 上游 LT_DS_FAST 条件结构（level-1 已启用；与 asi 原无条件常量版逐位等价，见 §3 事故 2 的后续修复） |
| `gpu/src/kernels.cpp` | 上游 suffix 参数（_lt/_lx）+ asi 的 u8 路径处理 |
| `runtime/include/opennr_rr_api.h` | 上游 level-4/on=2 文档 + asi 的 `ONR_RR_STATIC` 门控（上游无此机制） |
| `runtime/src/runtime.cpp` | 上游 net_lx 卸载 + asi 的守卫式 teardown/out16 销毁 |
| `runtime/validate/vk_main.cpp` | 上游 ONR_VALIDATE_DUMP + asi 除零守卫与非有限判定 |
| `README.md` | asi 自述保留 + 上游三模式表并入；集成说明改 v0.1.5 |

后续两个修复提交：
- `fix(asi): realign occ/b2 probe shader variants with v0.1.5 level-1 defines` — occ_nopad/pad16k 十行改到新 define 集（K2 哈希门禁要求 nopad 与 ship 逐字节一致）；b2_base 三内核镜像新 ship；b2_post_exp/log 保留原实验宏。
- `feat(asi): build the experimental _lx kernels in the plugin shader set` — build_shaders.py 解析 LX_DEFS，产出 4 个 _lx 变体（90→94 个 spv），OnrRrSetFastMode 2 可找到自己的内核；缺省仍按 1 跑。

门禁：build_asi.sh 全量构建 + `test_asi_units`（LLPC）全绿，0 FAIL（含 K2 哈希门禁）。

### 1.3 ACO 分支 rebase（最终线性 8 提交）
`git rebase asi-plugin`（7 pick，`65a64cf`/`cc82265` 作为空提交被 drop——其内容已被 cutover 提交吸收）。冲突 2 处：
- `enc0_attr.comp`：上游 `LT_ATTR_LATE` 与 ACO `ATTR_DEBUG_RAW` 同位插入 → 两个完整 #ifdef 并存（事故：首版合并重复了 ACO 侧自带的 #ifdef 行且残留 conflict marker，glslang 报 unterminated #ifdef，修复提交在案）。
- `README.md`：取新 asi 版 + 补回 198219d 的两处增量（aco 记录目录部署行、构建说明段）。

另两笔必要提交：
- `aco: route the embedded package capture through the 26.3.0-drirc toolchain` — build_asi.sh 内嵌打包段原指向 26.2.4 pin（`96cb4312`）与已归档删除的 `~/src/mesa`；改走 `build_capture_263.sh` + 其 toolchain.json/aco_capture + DRIRC_CONFIGDIR。
- `aco: make the downsample weight selects unconditional` — 见 §4 事故。

最终分支头见 §11。

## 2. 本地改动保留核对表

| 项 | 状态 | 证据 |
|---|---|---|
| B0：W1 不加 LDS_OCC_PAD | ✅ 保留（默认态） | 0.1.5 无任何构建入口设置 LDS_OCC_PAD；shader 侧旋钮仍在（onr_block.glsl:380、onr_block_w1.glsl:97），occ 探针仍可用 -D 复现 |
| B1：t0 尾部池化 + flow 审计 | ✅ 保留 | flow_ 分配/清零 10*1280*4（engine.cpp:326/612/614）；resize batch/need_hist 结构经 §1.2 合并保住 |
| engine.cpp 两项：flow 10*1280*4 清零；wide 用 create_unzeroed | ✅ 保留 | 前者见上；后者 engine.cpp 3 处 + d3d12.hpp/vk.hpp 接口 |
| Stage 1.5：enc96 单 tile + W2 精度策略 | ✅ 保留并复核（§5） | enc_stage_96_128_128.comp:13-17 |
| Stage 1.5：dec96 FC1 整数地址 | ✅ 保留并复核（§5） | dec_dfn_96_128.comp:10-14（FC1_ROW_TILE 守卫） |
| Stage 1.5：enc128 DontUnroll | ✅ 保留（复核中一度判惰性后反转，见 §5） | enc_stage_128_128_160.comp:14 |
| build_asi.sh cmat-address 仅两精确名 | ✅ 保留 | `--cmat-address-shaders dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt` |
| ACO runtime：D3D12 加载/strict/来源校验/描述符守卫 | ✅ 保留 | d3d12.cpp pin `0a6fa60a…`；`aco_table_lo_` 守卫 3 处在位 |
| ASI 侧：OptiFG/Streamline/DXGI 队列 | ✅ 保留 | asi merge 未触碰；LLPC 单元全绿 |
| B2 三宏与上游 HW/LDS/EXPO 共存性 | 源码结论：可共存 | 三宏与上游宏各自独立 #ifdef、不同代码区域；EXP 走专属 #elif 槽位，仅与未启用的 W16 互斥（onr_post_main.glsl:225/243/252/312/346/487）。实测后判定不启用（§5） |

## 3. 构建

- asi-plugin：90+4(_lx)=94 spv；openrr-amd.asi 3,241,236 B，导入表 6 DLL。
- ACO 分支：build/asi-015-aco（全部产物）+ build/aco-v015（trace runtime/bench/units）。
- level-2（_lx）可构建且不影响 level-1：4 个 _lx 记录入库（dec_head_32_64_f_lx 192 VGPR / dec_up_32_64_64_f_lx 216 / enc0_stage_32_64_f_lx 192 / enc0_stage_32_64_f_w_lx 192）。

## 4. 捕获与 ABI

- 全量捕获：**109 条记录（94 ship + 15 occ/b2），0 失败**；探针 4 条（abi-probe 目录）0 失败。mesa pin 与指纹链不变（release-tag commit + tarball/radv/patch/drirc SHA256 全入 manifest）。
- 事故 1（已修）：W_ODD 字面量池——0.1.5 的非 fast downsample 4 个变体在 ACO 26.3.0 下把 `W_ODD[5]` 常量数组落到代码尾部字面量池（恰 5 字：a8e7/35f4/38f6/2d96/a94b），严格记录格式（assembler-dual-sentinel）拒绝。修复 = wt() 常量 select 形式无条件化（常量与索引逐位相同；_lt 变体源码不变，仅 plain 变体字节变化）。
- ABI：探针 4/4（244B push ABI/车道可变 push/clear_range BDA/sampler 描述符槽），strict 全量加载，无未认领 user SGPR（drirc 抑制 CS_STATE 机制同前），scratch=0。
- 单元：`test_aco_units`（ACO 对比模式）ALL PASSED；LLPC 版 test_asi_units 0 FAIL。

## 5. Stage 1.5 / B2 复核（离线单因子，同工具链）

| 项 | 基线（保留） | 变体（关闭） | 判定 |
|---|---|---|---|
| cmat dec_dfn_96_128_fl_lt | 22272 B / 256 VGPR | 23796 B（off） | **保留**（−1524 B） |
| cmat enc_stage_128_128_160_fl_lt | 31972 B / 240 VGPR | 34104 B（off） | **保留**（−2132 B） |
| enc96 单 tile/W2=0（+SIG_RCP） | 36092 B / **240** VGPR | 29960 B / **256** VGPR | **保留**：+6132 B 代码换 VGPR 256→240，**跨过 240 台阶向下（每 SIMD 占用波 5→6）**，占用收益成立 |
| dec96 FC1 整数地址 | 23816 B（cmat off 口径） | 24648 B | **保留**（−832 B，VGPR 同） |
| enc128 DontUnroll | 31972 B（cmat on） | **47056 B**（cmat on，无 hint） | **保留**：hint 仅在 cmat-on 组合显效（+15084 B，与 0.1.4 时代 +47% 的结论方向一致）；cmat-off 时确实无影响（ELF 同哈希）——复核结论一度误判"惰性"，回滚源改动后记录恢复 31972/`0ce0c497…` 与生产逐字节一致 |
| B2 post 三宏（上游配置之上） | post_f_lt（ship，上游 HW/LDS/EXPO）12216 B / 96 VGPR | 同内核 +B2 三宏 12228 B / 96 VGPR | **不保留**：B2 为**有损近似**（折线 exp2/去角/去网络项），且 §9 的画质余量不足以再让一步；字节差 −12 B 本身无实益。level-1 维持上游配置，b2 fixture 目录留档 |

post 数字口径说明：本表 12216/12228 B 是**同一 ship 内核 post_f_lt**（上游 define 集）开/关 B2 三宏的 A/B；§7 表中 0.1.4 侧的 20812 B 是 0.1.4 ship post_f_lt（B2 宏开启的旧配置），而 0.1.4 manifest 里另有 20976 B（b2_post_exp fixture）、22168 B（b2_post_log fixture）等同名不同字节条目——跨版本对比必须按 ship key（fnv(stage spv)）严格匹配，本文 §7 已按该口径重算（初版 §7 的 post 行误比了 fixture 条目，已修正）。

## 6. 正确性门禁

Fast level-1 ACO vs LLPC（同 SPIR-V，parity-all-frames，guides=0）：

| 场景 | PSNR_01 | PSNR_TM | p99 | 非有限 | 判定（≥54/≥58/≤0.006/0） |
|---|---|---|---|---|---|
| 1080p steady | 60.64 | 65.25 | 0.00269 | 0 | PASS |
| 1080p DRS | 66.81 | 71.47 | 0.00146 | 0 | PASS |
| 4K steady | 61.34 | 66.15 | 0.00269 | 0 | PASS |
| 4K DRS | 61.59 | — | 0.00269 | 0 | PASS |

Legacy（fast-mode 0）：ACO vs LLPC **1080p/4K 全帧 100.0000% / ulp 0 位一致**。
跨版本：Legacy 0.1.5 vs aco-014-prod（旧 bench 驱动，向后 ABI 兼容附带验证）**1080p/4K 20+12 帧位一致**（上游"Legacy 输出位不变"在 ACO 管线下成立，含 downsample 常量 select 化的等价性证明）。
跨版本自检：Legacy 0.1.5 vs v0.1.2（MSVC 基线）1080p **逐位一致**【4K 见 §9 汇总】。
覆盖：trace 帧全部 25 个 dispatch **aco=1**，含 LT_UP8/LT_MI8 三内核、post 系列、_w 宽路径（d3d_enc0_attr_f_w_lt、enc0_stage_32_64_f_w_lt）；post_dlt 由单元 R 项覆盖。

**已知问题（发布前必须解决）**：guides=1（全可选引导）下 ACO/LLPC 出现数值敏感差异——本轮 Fast+guides 短测 p99 0.00854、PSNR_01 55.09。这是 `mesa-guide-isolation-2026-11-11.md` 已记录的 ACO/LLPC 数值敏感问题在 0.1.5 下的延续（0.1.4 全 guide 43.8 dB、LT_VO 放大），**不是**短 warmup 瞬态（统一口径复核：guides=1 稳态直接互比 1080p ~32.7 dB，见 §9.2 表 C——比 0.1.4 的 43.8 更差；Legacy @ guides=1 下 ACO/LLPC 仍近逐位一致（≥121 dB），分歧确认完全来自 _lt 近似内核）。本轮不修，列入发布前必须解决项；正式结论均基于 guides=0。

## 7. 记录对照（vs 0.1.4-drirc 生产记录，ship key 严格匹配）

Top-6 `_f_lt`：

| 内核 | 0.1.4 B/VGPR | 0.1.5 B/VGPR | Δ |
|---|---|---|---|
| enc0_stage_32_64 | 23300/192 | 25052/192 | +1752 B |
| post | 20812/120 | **12216/96** | **−8596 B、−24 VGPR**（上游 post 重写） |
| dec_up_32_64_64 | 24912/216 | 23860/216 | −1052 B |
| dec_head_32_64 | 21244/192 | 24120/192 | +2876 B |
| enc_stage_64_64_96 | 31272/240 | 33204/240 | +1932 B |
| dec_up_64_64_96 | 25840/240 | 27516/240 | +1676 B |

VGPR 台阶：Top-6 内 enc96 复核项跨 240 台阶向下（§5）；全集仅 post/post_dlt 因上游重写跨 120 台阶向下（120→96，占用 6→7 波）。其余 88 个同名内核 VGPR 持平。
全集（90 个同名 ship 内核）：代码量 1850176 → 1834848 B（**−0.83%**）；22 个内核字节有变（post −8596、post_dlt −8572、downsample_tl_lt −1548、d3d_enc0_attr −1744 等 vs enc0_stage +1752、dec_head +2876、enc_stage_96 +1932 等）；+4 个 _lx 记录（192/216 VGPR）。

FP8 WMMA（**llvm-mc 按编码长度解析最终 ISA**，严格 ship key；初版 IR 文本计数约为真值 2 倍，已废弃）：

| 内核 | 总指令 | v_wmma_f32_16x16x16_fp8_fp8 | v_wmma_f16_16x16x16_f16 |
|---|---|---|---|
| dec_up_32_64_64_f_lt | 3205 | **32** | 255 |
| dec_up_64_64_96_f_lt | 3668 | **48** | 415 |
| enc_stage_64_64_96_f_lt | 4462 | **64** | 472 |
| dec_head_32_64_f_lt | 3630 | 0 | 196 |
| enc0_stage_32_64_f_lt | 3594 | 0 | 257 |
| post_f_lt | 2074 | 0 | 0 |

→ LT_UP8/LT_MI8 在 ACO 下确实生成原生 FP8 WMMA；三内核仍保留大量 f16 WMMA（Stage 2 观察：FP8 化未覆盖的乘积层即在此）。

## 8. v0.1.2 冻结基线

- 采纳：**MSVC 构建**（源 = 纯 tag `f75fadb`，零补丁；cl 19.44.35207 /std:c++20 /O2；vulkan-1.lib 由 74 符号 def 经 `lib /def:` 生成；178 个 spv 复用同 tag 的 WSL glslang 产物）。自检：bench 加载、40 条 fast 管线自建 13.3s、干净销毁。`baselines/v0.1.2-msvc/`（SHA256SUMS 187 文件，DLL `8a65797f…`）。
- 弃用留档：MinGW 交叉版（`baselines/v0.1.2/`）能加载但 Vulkan 设备创建后 C0000005（两代 bench 均同），六个 MSVC 兼容补丁见其 BUILD-NOTES；不作为基线。
- 旧画质数据点作废说明：原"Fast 1080p 对 v0.1.2 = 50.48 dB"为单帧单点口径（稳态末帧读回、无逐帧），同参数重跑得 59.96 dB、统一口径得 52.06 dB——跨版本 PSNR 对历史演化与调度扰动混沌敏感（±10 dB 量级），单点数字无意义；统一口径三统计与逐帧曲线见 §9.2 表 B，50.48 的归因见 §9.6。

## 9. 新基线测量

### 9.0 跨版本管线自检
Legacy 0.1.5 vs v0.1.2（MSVC）：1080p 与 4K 均**逐位一致**（exact 100.0000% / ulp 0，parity-all-frames）→ 测量管线（基线加载、帧供给、读回比对）可信。

### 9.1 端到端测速（Long：48 warmup / 240 steady / 64 DRS；6 交替轮配对，双侧同进程先后）

| 单元 | 分辨率 | 对照侧中位 | 0.1.5 侧中位 | Δ | 方向 |
|---|---|---|---|---|---|
| a：0.1.4-ACO → 0.1.5-ACO（Fast） | 1080p | 3.497 | 3.361 | **−3.88%** | 6/6 更快 |
| a（Fast） | 4K | 12.944 | 12.380 | **−4.35%** | 6/6 更快 |
| b：LLPC → ACO（Fast，同一 runtime/记录换编译器） | 1080p | 3.009 | 3.355 | ACO +11.48% 慢 | 0/6 |
| b（Fast） | 4K | 11.389 | 12.300 | ACO +8.00% 慢 | 0/6 |
| a（Legacy） | 1080p | 3.892 | 3.899 | +0.20% | 0/6 |
| a（Legacy） | 4K | 14.460 | 14.570 | +0.76%（p95 16.25→17.01） | 2/6 |
| b（Legacy） | 1080p | 3.468 | 3.899 | ACO +12.41% 慢 | 0/6 |
| b（Legacy） | 4K | 13.123 | 14.508 | ACO +10.55% 慢 | 0/6 |

要点：
- **版本净收益（a）**：Fast 双分辨率 −3.9%/−4.4%（6/6）。**注意口径**：0.1.4 生产线 post/post_dlt 带 B2 三宏，0.1.5 不带（上游 HW/LDS/EXPO 替代），净收益里含这部分替换的影响，不纯粹是"上游改进"。
- **ACO vs LLPC（b）**：0.1.5 上 ACO 慢 8–12.4%。对照 0.1.2 时代未调优时的 −16~18%，**差距收窄约 5–7 个百分点**（Stage 1.5 保留项 + 0.1.5 内核变化混合效果）；方向未反转，ACO 仍落后 LLPC。Legacy 侧 ACO 落后 10.6–12.4%（Legacy 无 level-1 保留项加持）。
- DRS 同向：Fast a −4.1%/−4.6%，b ACO +9.8~11.1% 慢。

### 9.2 画质（统一口径重测，v2 —— 旧表作废，见 §9.6）

统一参数：warmup 32 / steady 32 / DRS 8 / parity-all-frames / 逐帧 PSNR_01·PSNR_TM·p99；三个统计量 = **全程最差帧 / 稳态末帧 / DRS 段最差帧**。bench 默认（不开 all-frames 时）只比"稳态末帧 + DRS 末帧"两帧（`steadyPixels/drsPixels` 段尾读回）——旧数据的口径混乱即源于此。v0.1.2 侧因无 OnrRrDiagnostic 不能相位对齐（由内容确定性保证可比，同参数跨进程帧逐位一致已验证）；0.1.5 内部对比全部带 align。

**表 A：画质锚点 = 0.1.5 Fast(level 1) 对 0.1.5 Legacy（上游自接受的 Fast 损失，画质预算起点）**，PSNR_01 dB：

| guides | 分辨率 | ACO 最差/末帧/DRS最差 | LLPC 最差/末帧/DRS最差 |
|---|---|---|---|
| 0 | 1080p | 38.15（steady_3 瞬态）/ 49.53 / 58.04 | 38.29 / 49.51 / 58.04 |
| 0 | 1440p | 57.67 / 58.75 / 57.67 | 57.67 / 58.75 / 57.67 |
| 0 | 4K | 45.85（steady_0 瞬态）/ 59.41 / 49.01 | 45.86 / 59.39 / 49.01 |
| 1 | 1080p | 31.22 / 32.00 / 43.23 | 29.03 / 29.69 / 43.23 |
| 1 | 1440p | 29.25 / 29.25 / 41.83 | 29.25 / 29.25 / 41.83 |
| 1 | 4K | 33.93 / 33.97 / 43.29 | 33.92 / 33.97 / 43.45 |

→ ① ACO 与 LLPC 的锚点值几乎重合（差 ≤0.6 dB）：Fast-vs-Legacy 的损失与编译器无关。② guides=0 时 Fast 损失约 49.5–59.4 dB（分辨率相关，1080p 最弱且带 38 dB 前几帧瞬态）；**guides=1 时坍到 ~29–34 dB**——全引导内容把 Fast 的近似放大一个数量级，这是画质预算的真正边界。

**表 B：对冻结 v0.1.2（历史参照；历史混沌敏感，见 §9.6）**，PSNR_01 dB：

| guides | 分辨率 | ACO 最差/末帧/DRS最差 | LLPC 最差/末帧/DRS最差 |
|---|---|---|---|
| 0 | 1080p | 46.19 / 52.06 / 50.69 | 58.85 / 58.85 / 59.92 |
| 0 | 1440p | 59.81 / 60.97 / 59.81 | 59.81 / 60.97 / 59.81 |
| 0 | 4K | 58.44 / 61.14 / 58.44 | 58.44 / 61.13 / 58.44 |
| 1 | 1080p | 51.91 / 64.49 / 51.91 | 52.10 / 64.51 / 52.10 |
| 1 | 1440p | 50.79 / 64.34 / 50.79 | 50.79 / 64.34 / 50.79 |
| 1 | 4K | 50.13 / 62.75 / 50.13 | 49.84 / 62.87 / 49.84 |

→ 除 g0-1080p 外 ACO/LLPC 对 v0.1.2 基本重合；g0-1080p 的 ACO 曲线呈 46→55 dB 的历史瞬态（LLPC 平直 58.9）——ACO 在该配置的历史轨迹更慢收敛，逐帧曲线见 `q2/q2-summary.json`。

**表 C：ACO 对 LLPC 直接对比（同 SPIR-V，align）**，PSNR_01 dB：

| guides | 分辨率 | 最差/末帧/DRS最差 | 备注 |
|---|---|---|---|
| 0 | 1080p | 58.96 / 59.52 / 70.55 | steady 平直 59–60.5；DRS 段 7/8 帧逐位一致 |
| 0 | 1440p | steady 全 999 / drs_7 53.82 | 稳态 ACO≡LLPC 逐位一致 |
| 0 | 4K | 60.54 / 61.12 / 60.54 | |
| 1 | 1080p | **32.22 / 32.74** / 61.00 | steady 平直 ~33（比 0.1.4 的 43.8 更差）；DRS 段反而 61–133 |
| 1 | 1440p | steady 全 999 / drs_7 53.82 | 同 g0 |
| 1 | 4K | 53.40 / 62.31 / 53.40 | |

补充检验（补测格）：**Legacy @ guides=1 的 ACO vs LLPC = 1080p/4K 全程最差 121.3/122.1 dB**（稳态末帧 127.9/127.3，仅 DRS 帧个别半 ULP）——Legacy 精确路径在两编译器下确定一致（guides=0 此前已验，guides=1 现补验）。由此归因钉死：**guides=1 的 33 dB 分歧完全经 `_lt` fast 内核的近似进入**（LT_VO 嫌疑），与 Legacy 路径无关。
→ **改正旧结论**：原文"ACO/LLPC 敏感性远小于两者对 v0.1.2 的距离"方向写反且论据错误（PSNR 不满足三角不等式，误差方向可部分抵消：g1-1080p 直接互比 32.7 dB 的两侧，各自对 v0.1.2 却都是 64.5 dB，数学上完全自洽）。正确表述：**guides=1 下 ACO/LLPC 的直接分歧（steady ~29–33 dB@1080p/1440p 锚点口径、32.7 dB 直接口径）远大于 guides=0（~59.5+），是发布前必须解决的数值敏感项**；g0 下分歧很小且 DRS 段多处逐位一致。1440p 的稳态逐位一致与 1080p 的 33 dB 并存说明该敏感性与内容和分辨率强相关。

### 9.3 逐核耗时（v3 —— ONR_RR_FLOW=0 真串行重测；v2 的"删 `_fl_lt`"法作废，见 §9.6）

**方法**：保留全部 `*_fl_lt.spv` 不动，设 `ONR_RR_FLOW=0` + `ONR_RR_STAMPS=1`（PowerShell 进程内设置；WSL→Win exe 环境变量不透传），48 warmup + 600 稳态帧，取日志第二个 flush 块（~598 帧均值）。解析严格按 `stamped dispatches:` 的 `kernel.spv@idx` 取 `intervals[int(idx)]`（偶数槽=算子本体，奇数槽=间隔）。Fast 模式 21 个 dispatch、45 个区间；Legacy 21/45 同构。
**自检**：4 组（2 分辨率×2 编译器）"全部区间和 vs network 均值"偏差 ≤0.2%（如 1080p Fast ACO 3.459 vs 3.46 ms），加和性成立。

为什么 v2 不成立：`engine` 判定 flow 用的是基础核表 `K.has("*_fl")`，只删 `_fl_lt` 时 Legacy `*_fl` 仍在 → flow=1，Fast 静默回退 Legacy `_fl` 并发路径。v2 表中内核名全为 `_fl` 即直接证据；其结论（"劣势集中 dec_up 32/64 形状"）实为并发错记 + 顺序 zip 双重错误的产物。本节为同栈 `1f8038d`（=冻结基线 `v0.1.5-aco-base`）上的权威数据。

端到端（间隔全和 / network）：Fast 1080p **3.459/2.994 ms=1.155×**、4K **12.934/11.441=1.130×**；Legacy 1080p 3.925/3.470=1.131×、4K 14.683/13.165=1.116×。Fast 全 21 核（ms，`_f_lt` 真串行）：

| 内核 | 1080p ACO | 1080p LLPC | 比 | 4K ACO | 4K LLPC | 比 | 4K Δms |
|---|---:|---:|---:|---:|---:|---:|---:|
| enc0_stage_32_64_f_lt | 0.3639 | 0.3487 | 1.04 | 1.4268 | 1.4045 | 1.02 | +0.022 |
| post_f_lt | 0.3257 | 0.2880 | 1.13 | 1.2848 | 1.1494 | 1.12 | +0.135 |
| dec_head_32_64_f_lt | 0.3047 | 0.2920 | 1.04 | 1.2600 | 1.2120 | 1.04 | +0.048 |
| dec_up_32_64_64_f_lt（FP8） | 0.3036 | 0.3001 | 1.01 | 1.2127 | 1.2194 | 0.99 | −0.007 |
| enc_stage_64_64_96_f_lt（FP8） | 0.2747 | 0.2639 | 1.04 | 1.0086 | 1.0166 | 0.99 | −0.008 |
| d3d_enc0_attr_f_lt | 0.2298 | 0.1930 | 1.19 | 0.9313 | 0.8460 | 1.10 | +0.085 |
| dec_up_64_64_96_f_lt（FP8） | 0.2243 | 0.2183 | 1.03 | 0.8446 | 0.8456 | 1.00 | −0.001 |
| dec_dfn_64_64_f_lt（FP8QV） | 0.2041 | 0.2009 | 1.02 | 0.7849 | 0.7911 | 0.99 | −0.006 |
| **enc_stage_96_128_128_f_lt** | 0.1886 | 0.1414 | **1.33** | 0.6642 | 0.5095 | **1.30** | +0.155 |
| **dec_dfn_96_128_f_lt** | 0.1636 | 0.1190 | **1.37** | 0.6134 | 0.4355 | **1.41** | +0.178 |
| dec_up_96_128_128_f_lt | 0.1599 | 0.1508 | 1.06 | 0.5626 | 0.5396 | 1.04 | +0.023 |
| **enc_stage_128_128_160_f_lt** | 0.1139 | 0.0709 | **1.61** | 0.3922 | 0.2324 | **1.69** | +0.160 |
| **dec_up_128_128_160_f_lt** | 0.0970 | 0.0649 | **1.49** | 0.4057 | 0.2171 | **1.87** | +0.189 |
| **dec_dfn_128_128_f_lt** | 0.0846 | 0.0552 | **1.53** | 0.3835 | 0.1877 | **2.04** | +0.196 |
| dec_up_160_256_160_f_lt | 0.0893 | 0.0570 | **1.57** | 0.1872 | 0.1218 | **1.54** | +0.065 |
| dec_dfn_160_256_f_lt | 0.0867 | 0.0505 | **1.72** | 0.1643 | 0.1082 | **1.52** | +0.056 |
| enc_stage_160_256_160_f_lt | 0.0844 | 0.0572 | **1.48** | 0.1813 | 0.1245 | **1.46** | +0.057 |
| enc_stage_160_256_0_f_lt | 0.0580 | 0.0362 | **1.60** | 0.0817 | 0.0529 | **1.54** | +0.029 |
| d3d_downsample_lt | 0.0583 | 0.0471 | 1.24 | 0.2652 | 0.2618 | 1.01 | +0.003 |
| d3d_ae / pooled_history2_lt | 0.0223 | 0.0221 | 1.01 | 0.0928 | 0.0915 | 1.01 | +0.001 |

结论（v3，替换 v2 全部单核结论）：
- **全部六个 W1/W2 大核（含全部已启用 FP8 的核）ACO 与 LLPC 打平（0.99–1.04×）**：dec_up_32/64 两个 FP8 形状 4K 甚至 0.99×，dec_dfn_64 0.99×——v2 的"dec_up 32/64 形状 1.39–2.06× 劣势"是解析错误，不存在。
- **ACO 端到端差距（Fast 4K +1.49 ms）的 ~74%（+1.107 ms）在深层 10 核（enc96..dec_dfn_96）**；其中 C=128 三核最差（1.69/1.87/2.04×，合计 +0.545 ms @4K），C=96 enc/dfn 次之（1.30/1.41×），160 家族 1.46–1.60× 但绝对量小。1080p 深层 10 核 +0.323 ms（占 ~69%）。
- 其余：post_f_lt +0.135 ms @4K（1.12×）、d3d_enc0_attr_f_lt +0.085 ms（1.10×）、dec_head +0.048 ms；1080p 特有 downsample +0.011 ms（1.24×）。
- Legacy 同型退化（enc128 1.63×、dec_dfn_128 2.01×、du128 1.88×）→ **深层劣势与 `_lt`/FP8 无关，是 ACO 对这些形状的代码生成/寄存器策略问题**（与 rsrc1 VGPR 台阶假设一致，待 Step 2 ISA 核查）。
- Stage 2 目标排序（v3）：① C=128 三核 ② dec_dfn_96/enc96 ③ 160 家族 ④ post_f_lt ⑤ d3d_enc0_attr_f_lt。


### 9.4 显存（6 轮中位）
全部分辨率/单元/编译器组合：**feature 207.0 MB（1080p）、789.0 MB（4K）；进程增量 229.1 / 810.3 MB**——与 0.1.4 生产完全一致，0.1.5+重捕零显存变化。

### 9.6 测量方法修正（v2 重测依据）

**问题一：flow 下逐核计时无效，且 v2 的"删 `_fl_lt`"串行化法同样无效（§9.3 v1/v2 均作废，v3 为准）**
`_fl_` 内核走 flow 路径，相邻 dispatch 之间没有 barrier、并发执行，ONR_RR_STAMPS 的时间戳间隔把耗时记到相邻内核上（v1 错误：enc_stage_64_64_96 记 0.0000 等）。v2 改用"删除 10 个 `_fl_lt.spv`"，但 `engine` 判定 flow 检查的是基础核表 `K.has("*_fl")`——Legacy 的 `*_fl.spv` 仍在，flow 保持 1，Fast **静默回退到 Legacy `*_fl` 并发路径**（v2 表内核名全为 `_fl` 即证据）；且 v2 解析仍用"核名×区间顺序 zip"，而正确做法是按 `stamped dispatches:` 的 `kernel.spv@idx` 显式取 `intervals[int(idx)]`（21 dispatch 产生 45 个区间，区间全和恒等于首尾时间戳差，加和自检无法发现错位）。双重失真。
修正（v3）：保留全部 `*_fl_lt.spv`，`ONR_RR_FLOW=0` 强制带屏障 `_f_lt` 路径 + `ONR_RR_STAMPS=1`，按 `@idx` 解析；自检区间全和 vs network 偏差 ≤0.2%。方向性结论反转：劣势集中点不是 dec_up 32/64 形状（实测 0.99–1.04× 打平），而是 C=128 深层三核（1.69–2.04×）与 C=96/160 家族 + post/enc0_attr。旧 v2 结论全部作废。

**问题二：画质采样口径不一致（旧 §8/§9.2 作废）**
bench 默认只比"稳态末帧 + DRS 末帧"两帧；旧数据各轮 warmup/帧数不同，末帧的历史收敛深度不同，出现 50.48 vs 59.21（对 v0.1.2，同配置不同帧深）与 55.09 vs 64.55×2（表面三角矛盾）这类不可同时成立的数字。
修正：统一 warmup 32/32/8 + parity-all-frames，报三统计 + 逐帧曲线。关键验证：
1. **同参数跨进程逐位可复现**（两个 v012 进程的 v0.1.2 帧逐位一致）→ 无底层随机性；
2. **align 与 no-align 的帧不同**（engine 相位改变历史演化）→ 跨参数组的比较无效，"三角矛盾"实为 align/不 align 两组数据各说各话；PSNR 本身不满足三角不等式（误差可部分抵消），表 C 的 32.7 dB 与表 B 的 64.5×2 并不矛盾；
3. **50.48 不可复现**：同参数重跑 59.96——单点跨版本 PSNR 对读回扰动/调度混沌敏感（读回会扰动调度，bench 源码注释明言），逐帧曲线（`q2/q2-summary.json`）才是有效信息；
4. v012-aco-g0-1080p 的 46→55 dB 历史瞬态（LLPC 平直 58.9）为真实编译器相关历史轨迹差异，非噪声。

**其他修正**：§11 分支头更新；可再生逐帧 bin 已清理（68 GB → 汇总 2 MB：summary/anchor JSON + 日志保留）。

## 10. 失败与诊断记录（按时间）

1. K2 哈希门禁 5 内核全挂 → build_asi.sh occ 段 0.1.4 define 集过时 → 对齐后 4/5；dec_head 仍挂 = 首次批量替换把 LT_UP8 也加进 dec_head 两行 → 修正后全绿（教训：define 前缀替换需按内核逐行核对）。
2. plain downsample 与 0.1.4 字节不同 → 0.1.5 源在 plain 路径引入 W_ODD 常量池（§4 事故 1）→ wt() select 无条件化。
3. enc0_attr.comp rebase 冲突首版：重复 #ifdef + 残留 conflict marker（`grep '^<<<<<<<'` 只查开头标记漏了结尾）→ 全仓三类标记扫描 + 平衡校验后修复。
4. build_asi.sh 内嵌 ACO 打包指向已删除的 26.2.4 源 → 切 build_capture_263.sh 流程。
5. v0.1.2 MinGW 构建连环兼容补丁（utility/share.h/PushFits/wstring ifstream/uuidof/bcrypt/-mf16c）后仍崩 → 改 MSVC 零补丁成功。
6. VS BuildTools 默认 MSVC 14.51 无 bin（残缺）→ `-vcvars_ver=14.44`。
7. 长测速脚本首版 48 轮全 rc=127（WSL 内用 `C:/` 路径调 exe）→ 调起用 `/mnt/c`、参数保持 `C:/`。
8. 环境既有坑沿用：PS 5.1 `1>` 重定向 UTF-16；运行目录不可复用；units 需 exe 旁 stage/b2/occ。

## 11. 交付清单

- 分支（均未推送）：`main` = `d9a3b2d`（= v0.1.5 tag）；`asi-plugin` = `975c5f3`（v0.1.5 merge `bdbbd0f` + occ/b2 define 对齐 `ab113ff` + _lx 构建 `975c5f3`）；`codex/mesa-263-comparison` = 本报告 v2 提交后的 HEAD（v1 为 `75fd141`；线上含 rebase 线性化的 ACO 线、捕获路由、downsample/enc0_attr 修复与报告）。
- 保护 tag：`aco-014-prod`（=原 992b6f8）、`asi-014`（=原 9cfa9d7）。冻结基线：`baselines/aco-014-prod/`、`baselines/v0.1.2-msvc/`。
- WSL 工具链核查：`~/work/openrr-mesa-263-comparison` 的 libvulkan_radeon.so SHA256 前缀 `7fc443f30e8abe99` 与本轮 toolchain.json/manifest 一致；基线冻结之后无任何 .so 重编（newer 计数 0）；`~/src/mesa-deps` pkgconfig 5 项在位；`~/archives/` 未动。
- 原始数据：`build/v015-runs/`（long + paired_summary.json / **q2**（统一口径画质：summary·anchor JSON·逐帧曲线，q2-summary.json）/ **noflow**（串行化逐核 + kernel_ms.json）/ profile3（作废版留档）/ asm / AB / legacyparity）；`build/asi-015-aco/`（109 记录 + abi-probe + capture-diag）；`build/aco-v015/`（trace runtime/bench/units）。逐帧 bin 已清理（q2 曾达 68 GB → 2 MB），summary/console/日志/trace 保留。

## 12. 复现命令（关键路径）

```bash
# 全量构建+捕获（26.3.0-drirc）
wsl -e bash -lc 'cd <repo> && bash asi/build/build_asi.sh build/asi-015-aco'
# trace 工具链
wsl -e bash -lc 'OPENRR_TRACE_BASE=build/asi-015-aco OPENRR_TRACE_OUT=build/aco-v015 bash asi/build/build_runtime_trace.sh'
# 门禁（示例：Fast 1080p ACO vs LLPC）
build/aco-v015/openrr_bench.exe --runtime build/aco-v015/openrr-trace.asi --baseline-runtime 同 \
  --data-dir build/asi-015-aco/stage/openrr-amd --baseline-data-dir 同 \
  --baseline-compiler llpc --candidate-compiler aco-strict --rw 1280 --rh 720 --ow 1920 --oh 1080 \
  --warmup 8 --frames 20 --drs-frames 12 --fast-mode 1 --optional-guides 0 --parity-all-frames 1 --align-engine-phase 1 --out <dir>
# ABI 探针
OPENRR_ACO_PROBE=1 OPENRR_SPV_DIR=<probe-shaders> ONR_ACO_DIR=<abi-probe records> test_aco_units.exe
# v2 串行化逐核（去 flow 数据目录 + stamps；自检加和 <5%）
cp -r build/asi-015-aco/stage/openrr-amd build/v015-runs/noflow-data  # 并删 *_fl_lt.spv
ONR_RR_STAMPS=1 build/aco-v015/openrr_bench.exe --runtime build/aco-v015/openrr-trace.asi \
  --data-dir build/v015-runs/noflow-data --baseline-compiler aco-strict --candidate-compiler aco-strict \
  --rw 1280 --rh 720 --ow 1920 --oh 1080 --warmup 10 --frames 1250 --fast-mode 1 --out <dir>
# v2 统一口径画质（示例：锚点 Fast-vs-Legacy 的 Legacy 侧；Fast 侧即 direct 轮 candidate 帧）
build/aco-v015/openrr_bench.exe --runtime build/aco-v015/openrr-trace.asi --data-dir <data> \
  --baseline-compiler aco-strict --candidate-compiler aco-strict --rw 1280 --rh 720 --ow 1920 --oh 1080 \
  --warmup 32 --frames 32 --drs-frames 8 --parity-all-frames 1 --align-engine-phase 1 --fast-mode 0 --optional-guides 0 --out <dir>
python build/v015-runs/psnr_frames.py <a.bin> <b.bin>          # 离线逐帧 PSNR（与 bench 公式一致）
python build/v015-runs/anchor_compare.py <fast_dir> <legacy_dir> aco   # 锚点逐帧比较
# v0.1.2 基线
--baseline-runtime baselines/v0.1.2-msvc/runtime/OpenNrRrRuntime.dll --baseline-data-dir baselines/v0.1.2-msvc/runtime --baseline-compiler env
```
