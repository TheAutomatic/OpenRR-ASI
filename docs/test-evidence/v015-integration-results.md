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

**已知问题（发布前必须解决）**：guides=1（全可选引导）下 ACO/LLPC 出现数值敏感差异——本轮 Fast+guides 短测 p99 0.00854、PSNR_01 55.09。这是 `mesa-guide-isolation-2026-11-11.md` 已记录的 ACO/LLPC 数值敏感问题在 0.1.5 下的延续（0.1.4 全 guide 43.8 dB、LT_VO 放大），**不是**短 warmup 瞬态。本轮不修，列入发布前必须解决项；正式结论均基于 guides=0。

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
- 首个画质数据点（Fast 1080p，guides=0，0.1.5-ACO 对 v0.1.2）：PSNR_01 50.48 / TM 54.87 / p99 0.00757（8 帧短测、跨版本口径，不适用编译器门禁；完整矩阵见 §9）。

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

### 9.2 画质余量 vs v0.1.2（guides=0；guides=1 见下表）

| 编译器 | 分辨率 | PSNR_01 | PSNR_TM | p99 | 最差帧 max | 非有限 |
|---|---|---|---|---|---|---|
| ACO | 1080p | 59.21 | 63.60 | 0.00317 | 0.01465 | 0 |
| ACO | 1440p | 60.99 | 65.84 | 0.00269 | 0.00928 | 0 |
| ACO | 4K | 61.22 | 66.04 | 0.00269 | 0.02075 | 0 |
| LLPC | 1080p | 59.27 | 63.66 | 0.00317 | 0.01196 | 0 |
| LLPC | 1440p | 60.99 | 65.84 | 0.00269 | 0.00928 | 0 |
| LLPC | 4K | 61.21 | 66.03 | 0.00269 | 0.02417 | 0 |

→ ACO 与 LLPC 对 v0.1.2 的画质差 ≤0.06 dB：**画质差几乎全部来自 0.1.5 算法本身，与编译器无关**。
guides=1（全可选引导，同口径对 v0.1.2；rc=1 为跨版本比较触发 bench 编译器门禁，数据有效）：

| 编译器 | 分辨率 | PSNR_01 | PSNR_TM | p99 | 最差帧 max | DRS PSNR_01 |
|---|---|---|---|---|---|---|
| ACO | 1080p | 64.55 | 71.81 | 0.00146 | 0.00391 | 53.28 |
| ACO | 1440p | 64.28 | 71.55 | 0.00171 | 0.00537 | 51.09 |
| ACO | 4K | 62.56 | 69.83 | 0.00244 | 0.00830 | 51.03 |
| LLPC | 1080p | 64.55 | 71.81 | 0.00146 | 0.00439 | 53.15 |
| LLPC | 1440p | 64.28 | 71.55 | 0.00171 | 0.00537 | 50.96 |
| LLPC | 4K | 62.51 | 69.76 | 0.00244 | 0.00732 | 50.95 |

→ guides=1 下 ACO/LLPC 对 v0.1.2 同样几乎重合（差 ≤0.05 dB / ≤0.001）：§6 的 ACO/LLPC 直接互比敏感性问题（43.8 dB 级）远小于两者对 0.1.2 的共同距离；该敏感项仍列发布前必须解决。

### 9.3 逐核耗时（ONR_RR_STAMPS，1250 帧，间隔均值；stamps 会略微放大帧时间，占比与相对差有效）

Top-8（1080p 间隔和 ACO 3.365 ms vs LLPC 3.052 ms，**+10.3%**；4K 12.980 vs 11.719，**+10.8%**——与端到端 b 单元自洽）：

| 内核 | ACO 1080p | LLPC 1080p | ACO 4K | LLPC 4K |
|---|---|---|---|---|
| dec_up_160_256_160_fl_lt | 0.3635 | 0.3500 | 1.4457 | 1.3982 |
| dec_up_128_128_160_fl_lt | 0.2696 | 0.2644 | 1.0206 | 1.0169 |
| enc_stage_160_256_160_fl_lt | 0.2328 | 0.1947 | 0.9382 | 0.8509 |
| dec_up_96_128_128_fl_lt | 0.1667 | 0.1699 | 0.5769 | 0.5838 |
| dec_up_32_64_64_f_lt（FP8） | 0.0888 | 0.0614 | 0.2136 | 0.1291 |
| post_f_lt | 0.0770 | 0.0476 | 0.0726 | 0.0460 |
| dec_up_64_64_96_f_lt（FP8） | 0.0741 | 0.0588 | 0.2447 | 0.2240 |
| enc_stage_96_128_128_fl_lt | 0.0150 | 0.0143 | 0.0551 | 0.0531 |

LT_UP8/LT_MI8 三内核的 ACO 对 LLPC 差距：
- dec_up_32_64_64：1080p **+44.6%**、4K **+65.4%**（FP8 WMMA 路径 ACO 劣化最重，LLPC 的 fp8 代码生成优势明显）
- dec_up_64_64_96：1080p +26.0%、4K +9.2%
- enc_stage_64_64_96：stamps 间隔记 0.0000（槽位时间戳与相邻重叠，间隔不可分辨，耗时被邻核吸收）——Stage 2 精确量化需单独引擎级工具
- post_f_lt（非 FP8，上游重写后 −24 VGPR）：ACO 慢 61–62%（1080p 0.0770 vs 0.0476）——上游新 post 在 ACO 26.3.0 的劣势是端到端 gap 的第三大来源

### 9.4 显存（6 轮中位）
全部分辨率/单元/编译器组合：**feature 207.0 MB（1080p）、789.0 MB（4K）；进程增量 229.1 / 810.3 MB**——与 0.1.4 生产完全一致，0.1.5+重捕零显存变化。


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

- 分支（均未推送）：`main` = `d9a3b2d`（= v0.1.5 tag）；`asi-plugin` = `975c5f3`（v0.1.5 merge `bdbbd0f` + occ/b2 define 对齐 `ab113ff` + _lx 构建 `975c5f3`）；`codex/mesa-263-comparison` = `2adc957`（rebase 线性化后的 ACO 线 + 捕获路由 + downsample/enc0_attr 修复 + 本报告）。
- 保护 tag：`aco-014-prod`（=原 992b6f8）、`asi-014`（=原 9cfa9d7）。冻结基线：`baselines/aco-014-prod/`、`baselines/v0.1.2-msvc/`。
- WSL 工具链核查：`~/work/openrr-mesa-263-comparison` 的 libvulkan_radeon.so SHA256 前缀 `7fc443f30e8abe99` 与本轮 toolchain.json/manifest 一致；基线冻结之后无任何 .so 重编（newer 计数 0）；`~/src/mesa-deps` pkgconfig 5 项在位；`~/archives/` 未动。
- 原始数据：`build/v015-runs/`（long/quality/profile3/asm/AB/legacyparity + paired_summary.json）；`build/asi-015-aco/`（109 记录 + abi-probe + capture-diag）；`build/aco-v015/`（trace runtime/bench/units）。逐帧读回按"可再生"清理，summary/console/日志/trace 保留。

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
# v0.1.2 基线
--baseline-runtime baselines/v0.1.2-msvc/runtime/OpenNrRrRuntime.dll --baseline-data-dir baselines/v0.1.2-msvc/runtime --baseline-compiler env
```
