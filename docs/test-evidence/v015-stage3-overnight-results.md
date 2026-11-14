# Stage 3 整夜棘轮（overnight）：五项正交攻坚全记录

基线 `bf57f97`（冻结整包 `build/v015-runs/overnight-stage3/frozen-bf57f97/`），棘轮头 **GREEN_HEAD = `be428c0`**（= bf57f97 数据 + 构建防呆 Commit 0）。整夜结论：**Phase A 六项全部实测后判负，零晋级**——GREEN_HEAD 的数据与冻结基线逐字节相同，Phase B 以退化 A/A 形式完成全绿终验。每一项的完整证据链与可复用补丁在 `build/v015-runs/overnight-stage3/item<N>/`。

## 0. Step 0：冻结与 Commit 0（`be428c0`）

- 冻结包断言：`manifest.json` 失败集空、cmat_address 去重 50 stem；stage 与冻结快照逐字节一致。
- **A/A 噪声地板**（3×ABBA 32/120/64，冻结对自身）：1080p `d_med=+0.119%`、4K `−0.179%`（组内最差 ±0.38%）。晋级线取 `−max(0.15%, 2×地板)`：**1080p −0.24%、4K −0.40%**。
- **锚点 #4 基线表**（ACO vs LLPC，frozen）：见 `anchor4/base-*`（g0 steady 58.92/61.06，1080-DRS 70.43，4K-DRS 59.85；g1 全部 120–137 dB）。
- Commit 0：`build_runtime_trace.sh` 全量同步 `aco-v015/stage`（旧影子包仅 2 核 cmat 的陷阱封死）；`build_asi.sh` 的 occ_nopad/occ_pad16k 探针宏改由 `fastmode.cmake` 的 `LT_DEFS_<stem>` 自动派生（HIST10 轮 K2 假失败的根因封死）。重编后 units 全绿 + 双 stage 哈希一致。

## 1. Phase A 六项战果

| 项 | 范围 | 判决 | 质量门 | 速度 d_med（1080p/4K）|
|---|---|---|---|---|
| 1 W1 MLP8 | enc0/dec_head/dec_up 32_64 | 判负（2 重试用尽）| min 帧 44.95→47.14（重置瞬态放大 K=32 激活量化）| 未达 |
| 2a TR 补漏 | dec_up_128/96 + enc96 fl | 判负 | 门 B 全绿 min 58.6 | −0.141% / −0.079%；锚点 #4 1080-DRS 70.43→63.22（确定性）|
| 2b W2 五核 | 64/96 家族 | 判负 | 全绿（1080-DRS 锁死 999.00）| −0.066% / +0.031% |
| 2c SPLIT2 | 160 四核 f+fl | 判负 | 全绿（1080-DRS 999.00）| −0.009% / −0.051% |
| 3 hist10 v2 | RGB10A2 分配期原子绑定 | 判负（格式精度地板）| steady 48.06/52.04 vs 54 门 | 显存 −36/−141 MB 实证 |
| 4a EMB8 | enc0 embed MLP | 判负（2 重试用尽）| 35.43 → 40.59（fc1-only）| 未达 |
| 4b QV8 | dec_head 旋转键 Q | 判负（速度中性）| **全绿** min 55.90/58.83；锚点 #4 8/8 格 | −0.123% / **+0.149%** |

**核心教训（跨项收敛）**：
1. FP8 增益不泛化：已船载的 128 家族 TR MLP8（`50bc902`）是唯一权重带宽受限面；W1/W2/SPLIT2/96/dec_up 全部速度中性或负。
2. 激活量化墙：C=32/K≤64 输入侧双侧 e4m3 ≈ 35–47 dB，逐帧 54 dB 门不可能；**softmax 后的 Q 面（4b）是唯一质量干净面**。
3. 4a 格式考古：ATTR64 记录字节是 fp16 高 8 位（e5m3 截断）而非 e4m3——`uintBitsToFloate4m3EXT(byte)` 直读 = 12.9 dB 灾难，正确路径是 unpack 后 `qe4`。
4. 4b 证明的机制（旋转键 A 尾 + 一次性 x8v B + QC8 分数累加 + softmax 吸收）是后续注意力侧 FP8 的可信模板，缺的只是足够大的表面。
5. hist10 机械层全部打通（spv 常量探针防混包、全或无绑定+闩锁、帧入口锁、sized_for_mode 接线、五场景单测），败因唯一：10-bit UNORM 对数域步长 0.0044 ≈ 0.31%/通道。重开方向：R11G11B10+逐帧偏置恢复符号、RGBE 共享指数、或放宽质量预算。
6. **构建管线缺陷（已修入流程）**：`build_asi.sh | tail -1` 管道吞非零退出码——4a 首轮着色器编译静默失败、门禁测的是陈旧树。棘轮命令现显式检查 `BASI-RC`。
7. Item 5（MRG8）**未跑**：前提不成立——计划的 merge 面映射到 `EMB1T/EMB2T`（ENC_EMBED-TR 遗迹，enc0 现为 W1 体不可达）与 `STEP_MI`（enc1 输入合并，LT_MI8 已船载原生 FP8）；深层输出合并的 [Co][4C] 瓦片消费面在时限内未能定位。留档为后续项。

## 2. Phase B 终验（GREEN_HEAD = be428c0，纯只读）

- **GREEN≡冻结**：spv 逐字节相同；109 记录 code_hash 全等；units ALL PASSED；Legacy vs 冻结 steady+DRS `exact 100.0000% / 0`。
- **长档 A/A**（3×ABBA 48/240/64）：1080p `+0.001%`、4K `−0.022%` —— 棘轮地板复证。
- **对同版 LLPC**（48/240/64 单实例）：ACO 3.180 / 11.799 ms vs LLPC 3.035 / 11.387 ms → **+4.78% / +3.61%**（门禁线 3.009/11.389：4K 仅超 +0.36%，1080p 超 +5.7%；LLPC 本轮自身 3.035/11.387）。
- **锚点 #2**（终验构建，四重断言全过）：v2_motion 5 案例 + v1 2 案例 `hash_gate=ref`、`non_finite=0`、`hash_gates` 全过；mean PSNR_01 41.6–49.4 dB（跨厂相似度口径，与 50bc902 同数据逐位一致）。
- **锚点 #4**：基线表即终值（GREEN 数据未变）。

## 3. 归档索引

- `frozen-bf57f97/` 冻结整包；`speed/aa*|pb*` A/A 与长档；`anchor4/` 锚点 #4；`anchor2/` 锚点 #2；`items/` 各项门禁原始 summary/log；`item{1,2a,2b,2c,3,4a,4b}/verdict.md + candidate.patch` 判决与可复用补丁。

## 4. 重开优先级建议

1. **4b 机制放大**：QV8 质量已证，把同模板推向 enc0/dec_up 的 Q/V 面（W1 头共享 `onr_w1_head.glsl`，一次改动三核受益）——注意 4b 在 dec_head 单核为速度中性，需三核一起测。
2. hist10 v2 的探针/绑定机械已就绪（`item3/candidate.patch`），等一个非 UNORM 的符号容器决策。
3. 4a 的 `2^k` 离线缩放（e4m3 次正规底 2^-9 截断小权重）单变量重试。


## 5. 白天补测与当前 @idx 逐核终局剖析

600 稳态帧 ×（ACO/LLPC）×（1080p/4K），`ONR_RR_FLOW=0 ONR_RR_STAMPS=1`，按 `stamped dispatches` 的 `@idx` 整数下标索引 intervals（ACO 候选侧 vs LLPC 候选侧；ACO 双侧交叉校验最大差 ≤0.005 ms）。全表 `build/v015-runs/day-idx/table.md` + `stamps.json`。

**差距解剖（4K +0.404 ms / +3.61%）**：
| 贡献源 | Δms | 比值 | 静态 |
|---|---|---|---|
| post_f_lt | +0.163 | 1.145× | 96 VGPR / LDS 8320 / exec 12216 |
| d3d_enc0_attr_f_lt | +0.093 | 1.111× | 96 VGPR / LDS 0 / exec 26716（rsrc1 bit29=1 WGP_MODE）|
| dec_up_160_256_160 | +0.033 | 1.279× | 192 VGPR / LDS 30208（CU_MODE，屏障 22×）|
| enc_stage_160_256_0 | +0.018 | 1.349× | 同上 |
| enc_stage_160_256_160 | +0.024 | 1.198× | 同上 |
| dec_dfn_128_128_f_lt | **−0.080** | **0.699×** | ACO 反超 30%（SPLIT1+XS_TRIM 后 LDS 9728）|
| 其余深层链 @14..@33 | ~+0.15 | 1.01–1.05× | 基本贴平 |

1080p 同构：+0.184 ms，post +0.041、enc0_attr +0.038、dec_dfn_160 +0.026（1.528×）、downsample_tl +0.011（1.238×）。

**白天三项实测判决**（全部按棘轮归档回退，树回 2118443）：
- **item3-tight（hist10 紧致区间）**：Span A [-0.05,3.05] 使 4K 过内建逐帧门（54.84/59.59），但锚点 #4 编译器交叉 g1 127→57-70 dB、锚点 #2 对 5090 多案例 −0.6~−1.1 dB、速度 +0.05~+0.07%（post 是采样/LDS 界非带宽界，@42 实测 0.332/1.342 ms 反超 GREEN 的 0.321/1.291）。Span B 探测词（−0.0f/3.0f）与冻结 spv 碰撞（假阳性→基线设备移除），弃。四维全负，归档 `item3-tight/`。
- **item1-dec-only（3a）**：每-tile 2-QC8 写法 + dont_unroll 后 dec_up vgpr 反降 216→192、质量全绿（min 帧 54.59——证实 enc0 输入层是唯一质量杀手）；但 ABBA +0.124/+0.092% 净负、锚点 #4 的 4k-g0-DRS −1.59 dB 超预算。归档 `item1-dec-only/`。
- **item4b-w1-all3（3b）**：QV8 三核全开，质量与锚点 #4 全绿（1080-g0 +0.7），ABBA −0.027/+0.005% 中性。归档 `item4b-w1-all3/`。

**结论**：W1/MLP/QV/hist10 表面上的 FP8 与带宽路线已系统性穷尽；剩余 LLPC 差距的 80% 集中在三个 codegen 面——post_f_lt（40%）、d3d_enc0_attr（23%）、SPLIT2 160 族（19%）——是下一轮（ACO 压核反超轮）的靶子。


## 6. 2c 晋级后的取舍记录与白天追加轮（`6ea5bde`）

**DLAA 画质取舍（裁定：保留 6ea5bde）**：160 族 SPLIT2 MLP8 复验晋级（对 fp16 同编译器路径
−1.147% @1080p / −0.608% @4K）。对 RTX 5090 同参考同用例验收（`build/v015-runs/s4-quality/`）：
v2_motion 五用例与 v1 Q_S0_4k_perf 全线持平（mean/min ≤0.10 dB）；**唯一取舍：v1
Q_S0_1080p_dlaa mean +0.22 dB（预算内）、最差帧 33.87→33.08（+0.79 dB，超 0.5 dB 线）**——
160 族 FP8 量化噪声经 DLAA 路径（无升采样稀释）在 ~33 dB 时序累积段的反馈放大。后续若收紧
DLAA 画质预算，重开面 = 160 族 FP8 的 dlaa 档守卫。

**差距度量（两口径，分开引用）**：
- **整帧差距（无打点配对测速，48/240/64，游戏等外部负载停止后）**：ACO 3.338 vs LLPC 3.266 →
  **+0.072 ms（+2.21%）@1080p**；ACO 11.773 vs LLPC 11.574 → **+0.199 ms（+1.72%）@4K**。
- **逐核耗时之和的差值（600 帧 @idx 打点，打点自身有扰动，值偏大）**：1080p +0.138 ms、
  4K +0.343 ms（`s4-quality/stamps.json`）。榜首：post_f_lt +0.164（1.145×）、
  d3d_enc0_attr +0.090（1.106×）、长尾 1.02–1.05×。两口径之差即打点扰动与核间间隙。

**B1.2 post BATCH（判负归档 `s4-b1/BATCH-REJECTED.md`）**：20 采样先发后混 0-ULP 成立
（四相位 exact 100.000000%/ulp 0），但 +3.31%/+2.96%——96→192 VGPR、s_wait_loadcnt 反而
20→95。**B2.1 enc0_attr 预取（判负归档 `s4-b2/REJECTED.md`，已按日志哈希更正旧记录口径）**：
0-ULP 成立，对真实基线（72fc19aa，18 装载/106 等待）等待 106→98，但 +0.21%/+1.80%。
**结论（收窄）：本次两种批量预取（post 采样批 / attr 装载批）都变慢——源级"先发后算"在
本仓这两核上不可用；下一步以同一份 SPIR-V 做 ACO vs LLPC 的生成对照定位 post/attr 的真实
差距来源（优先 post），找到具体差异后只试就地复用一类小刀（确认同址、不提前入寄存器）。**

**WGP（B1.1 判无效后暂缓）**：重做时应在 Mesa 编译时置 wgp_mode（让编译器一并生成对应的
同步指令，符合 GFX12 内存模型对 WGP 模式的语义要求），再核验实际加载的记录哈希与 rsrc1。
**160 族**：SPLIT=2 保持不变——4K 逐核差距合计仅约 0.002 ms（2c 已收割），本轮不动结构。


## 7. post_f_lt 差距定案：ACO 采样等待策略（同 SPIR-V 双编译器 ISA 对照）

取证方法：仓内现成的 `capture_llpc_hotspots.exe`（手编恢复）将同一份
`post_f_lt.spv` 在驱动 Vulkan 设备编译为 PAL ELF，`llvm-objdump -d -j .text` 反汇编
（`build/v015-runs/llpc-dump/`，含三核双编译器 ISA 全文 + POST_DIAGNOSIS.md）。

**PAL 元数据**：LLPC vgpr=88（ACO 96）、lds=8704（ACO 8320）、**wgp_mode=false（同为
CU_MODE，WGP 假设正式排除）**。

**核心差异**：同一 bicubic 5-tap，两侧都 5 连发 `image_sample_lz`，但等待策略——
ACO 4 次递减部分等待 `s_wait_samplecnt 0x3→0x2→0x1→0x0`（尝试逐样本流水消费），
LLPC 1 次总等待 `0x0` 后批消费全部 5 个。全局 ACO 36 wait vs LLPC 24。LLPC 指令数
多 16%（2405 vs 2074）却快 14.5%——等待减少的收益远大于指令膨胀。

**定性**：Mesa/ACO 调度器的 wait-insertion 策略差异，不是着色器源码排布问题。
三次源码侧修复（BATCH、WGP、attr 预取）判负的根因统一：源码无法控制 ACO 对已飞行
采样的等待插入。**修复面在 Mesa/ACO 侧**：当多个 image_sample 的消费者在相邻窗口时
合并为单次总等待。d3d_enc0_attr 侧（106 vs 109 wait，几乎持平）差距根源不同，暂未
定位。160 族按指示维持 SPLIT=2（残余逐核差 ~0.002 ms）。


## 8. Stage4 R1：两项单变量 Codegen 棘轮（E1 晋级 / E2 判负）

**E1 enc0_attr readonly BDA（晋级，`c75740e`）**：`LT_ATTR_RO=1` 把 expo/prm/tab 三个
纯标量 uniform 引用标 readonly（SPIR-V NonWritable，无 restrict）→ NIR/ACO 标
CAN_REORDER 并提升到 SMEM。ISA：global_load 108→71、s_load 69→100、s_wait_loadcnt
106→83、vgpr 96 持平、exec 26716→26284；仅 `_lt` 变体变，Legacy `_f` 哈希逐字节零变化。
门禁：units 全绿；四相位 exact=100.000000%/ulp 0/psnr 999（对 green0）；日志防 A/A
（基线 spv=72fc19aa vs 候选 5e2103a7）。速度：3×ABBA −0.903% @1080p / **−0.355%
@4K（三组全负）**；@6 单核 0.961→0.936 ms（**−25.6 μs**）。

**E2 post_f_lt 采样等待合并（判负，`build/v015-runs/s4-r1-item2-post-wait0/`）**：
Mesa 26.3.0 GFX12 `ONR_ACO_SAMPLE_WAIT0` 使 s_wait_samplecnt 16→**4（全 0x0）**，
采样发射不变（20）；仅 post_f_lt code_hash 变（95b8b8e1→93124126），其余全部记录
逐位不变；同 spv 异 code 日志核验真 A/B；0-ULP 全过。但速度零收益：干净 ABBA
+0.274% @1080p / +0.034% @4K，**@40 单核 600 帧 1.3228 vs 1.3227 ms（零变化）**。
阴性结论（修正 §7 推断）：ACO 的递减部分等待不花钱——首等待即等效全批等待；post 的
14.5% 差距不在等待条数（LLPC 指令更多仍更快），在随后 SALU/VALU 排布与 barrier 位置。
工具链已完整还原（Mesa 源 marker=0、post code_hash 复原 95b8b8e1、与 green1 零差异）。

**终盘整帧 ACO vs LLPC**（无打点 3×ABBA 48/240/64，同会话）：1080p ACO 3.327 vs LLPC
3.266 ms（+0.061 ms / +1.58%；ACO 侧较 2c 后实测 3.338 有改善）；4K 11.981 vs 11.658
（+0.322 ms / +2.70%，组内 2.78/2.70/2.62 自洽）。跨会话绝对值有 ±1% 漂移（外部负载），
单变量收益以组内 ABBA 与 @idx 微归因为准：**(bf57f97→c75740e) 已兑现 2c −0.61%@4K +
E1 −0.36%@4K**。


## 9. Stage4 R2：双地址 LDS 与编译期 CU_MODE（两项均判负，机制均验证成功）

### 9.1 R2-E1 `post_f_lt` 双地址 64-bit LDS 读取（`ONR_ACO_SHARED2_LOAD64`）

预检完全命中：32×`ds_load_b64` → **16×`ds_load_2addr_b64`**（DS 总载入 48→32），
`s_wait_dscnt` 31 不变；`code_hash` 95b8b8e1→**8b21f82b**，`record_sha256` 同步变，
manifest 中仅 post_f_lt 变化、其余记录与 GREEN_HEAD 逐位一致；units 全绿；0-ULP 四相位
100.000000%/ulp 0；实跑日志同 spv（d57398b3）异 code 且 d3d_enc0_attr 双侧全等（真 A/B）。

测速无收益：3×ABBA 1080p −0.372%（组 −0.41/−0.37/+0.22）、4K +0.031%（组不同向，噪声态）；
**@40 单核 600 帧 1.3263 vs 1.3285 ms = −2.2 μs（噪声级）**。结论：post 不受 LDS 吞吐限制
（31 次 dscnt 等待未减），合并不省时间。归档 `build/v015-runs/s4-r2-item1-post-lds2/`。

### 9.2 R2-E2 `d3d_enc0_attr_f_lt` 编译期 CU_MODE（`ONR_ACO_CU_MODE`）

LLPC 复核（当前 spv 5e2103a7 的 PAL ELF）：**wgp_mode=false（CU_MODE）**、vgpr 81、
LDS 0、18 装载/109 等待——对齐目标成立。

机制实现成功（根治 B1.1 假切换）：Mesa 编译期开关叠加在既有 capture EXPORT 条目上
（首次独立条目曾覆盖 EXPORT 致 117 个捕获静默失败，已修复为组合式）；仅
`d3d_enc0_attr_f_lt` 白名单：rsrc1 537657355(0x200c000b)→**786443(0x000c000b)**、
code_hash 不变（纯拓扑翻转）、record_sha256 变；其余记录逐位一致；**实跑日志铁证
基线 2×rsrc1=200c000b / 候选 2×000c000b**；0-ULP 四相位全过。

测速：**@6 单核 600 帧 0.9357→0.9232 ms = −12.5 μs（核内 −1.34%）**——真实改善，
回收剩余 @6 缺口的约 20%；ABBA 两轮均被外部负载污染（calls ±9~12% 离群，中位
−0.244/−0.960 与 −1.128/−5.026 方向为负但不可判级）；整帧推算核占帧 8% × −1.34%
≈ **−0.11%**，低于晋级线。按协议判负归档 `build/v015-runs/s4-r2-item2-attr-cumode/`，
标注为已证实核内微增益（未来与其它 enc0_attr 改动打包复验）。

**两份补丁均已 100% 还原**：Mesa 源零残留（cu=0、hook=2）、记录与 green 快照零差异
（`build/v015-runs/green-c75740e-stage/`）、树净 @ `a2b04ed`。

### 9.3 终盘整帧 ACO vs LLPC

待重启后干净环境补测（本轮 ABBA 全程受外部负载污染，不作为判级依据）。R1 收尾同口径
基线：1080p +0.061 ms/+1.58%（ACO 3.327 vs LLPC 3.266）；4K +0.322 ms/+2.70%
（11.981 vs 11.658，组内 2.78/2.70/2.62）。


## 10. Stage4 过夜棘轮（重启后干净环境）：三项判负 + 终盘同口径表

环境：重启后 GPU 空闲（0.9–1.1%），全部轮次 600 帧配对 `STAMPS=1 FLOW=0` 无 >0.06ms 外部抢占，
`@idx` 一律按同轮未改动核总耗时做频率漂移扣除（`build/v015-runs/idx_drift.py`）。
晋级门槛（本轮启用）：① 4K 目标核扣漂移净缩短 ≥8–10μs（≥1.0%，1080p 同向）；② 无打点 4K 3×ABBA
d_med<0 且 ≥2/3 组 ≤0。**三项均未过 ①。**

### 10.1 R2-E2 CU_MODE 干净复审判负（推翻昨晚 −12.5μs）

全链重建 + 全电池通过（rsrc1 786443、code 936d3c99 不变、日志 2×200c000b vs 2×000c000b、
units/0-ULP 全绿）。**@6 扣漂移：4K cand 0.9205 vs base 0.9200 = +0.53μs（无增益）**，
drift=1.0000；1080p −1.72μs。ABBA 4K −0.040%（组 +0.151/−0.066/−0.040）、1080p −0.036%。
昨晚 −12.5μs 与同期未改动核 @8/@38/@40 的 −13~−18μs 同源 = 频率漂移伪影。归档
`s4-overnight-item0-e2-cumode/`。

### 10.2 `post_f_lt` k 循环 dont_unroll（LT_POST_KLOOP）判负

预检命中且幅度巨大：code 95b8b8e1→dc393fa5、exec 12216→10236（−16.2%）、
**s_wait_dscnt 31→7**、指令 2074→1735、寄存器不变。门禁全绿（日志基线 d57398b3/95b8b8e1 vs
候选 06a053f9/dc393fa5）。但 4K @40 **+9.75μs（+0.75%，更慢）**、1080p −0.57μs。
结论：post 不是 LDS 等待受限；展开的冗余被 ACO 排成有效 ILP，滚环索引/分支更贵。
LDS 载入（R2-E1 合并）与 dscnt 等待（本轮）双方向证伪，post 勿再从 LDS/等待切入。
归档 `s4-overnight-item1-post-kloop/`（含双侧 asm/dis）。

### 10.3 DEC_UP 输入只读（LT_DEC_UP_RO）预检即判负

动机：`dec_up_64_64_96` ACO 231×b128 vs LLPC 91×b128+48×b64（数据量 2.4×），疑为别名阻碍载入复用。
按 E1 机制给 x/xlo/up_w/up_b 加 NonWritable 双子类型：**code_hash 1efd2f5d 逐位不变**
（指令 3668、b128 231、s_load 87 全同）。全核扫描补充：109 记录中**无任何 SGPR 寻址 global load**
——readonly 只对**标量 uniform** 提升有效（E1 的 37 次），对逐 lane BDA 向量载入无效，本仓已无第二处标量面。
归档 `s4-overnight-item2-dec-up-ro/`。

### 10.4 双编译器 ISA 画像（`build/v015-runs/isa-profile/`、`bigcore-llpc/`）

| kernel | ACO insn/glb/vgpr | LLPC insn/glb/vgpr | 4K ratio |
|---|---|---|---|
| enc0_stage_32_64 | 3451 / 89 / 192 | 3390 / 89 / 198 | 1.013 |
| dec_head_32_64 | 3455 / 84 / 192 | 3376 / 84 / 182 | 1.020 |
| dec_up_32_64_64 | 3264 / 120 / 216 | 2760 / 100 / 189 | 1.013 |
| enc_stage_64_64_96 | 4424 / 317 / 240 | 2439 / 135 / 241 | 1.007 |
| dec_dfn_64_64 | ~3.6k / 206 / 240 | 2065 / 98 / 240 | 1.006 |
| post_f_lt | 2074 / 22 / 96 | 2405 / 71 / 88 | 1.145 |
| d3d_enc0_attr_f_lt | 4495 / 71 / 96 | 4902 / 71 / 81 | 1.109 |

- 六大主干核：静态结构几乎相同、比值 1.006–1.02，ACO 指令更少仍略慢 → 调度细节（v_fmaak 多、
  dual-mov 多、s_delay_alu 多），源码不可达。
- 96 族 ACO 指令 1.7×、载入 2.2×，但运行仅 1.007 → 静态膨胀不在关键路径。
- post/attr：载入、等待、寄存器、占用（88 与 96 按 16 粒度对齐后同为 96 → 均 10 waves）全对齐，
  差距纯为指令排布；`v_min_num_f64_e32` 为 llvm-mc 21 对 gfx1201 新算子（min3/max3 f16）的误码，
  两侧同现，非真 fp64。

### 10.5 终盘整帧 ACO vs LLPC（唯一一次，48/240/64，干净环境）

| 分辨率 | ACO | LLPC | d | 组内（3 组） |
|---|---|---|---|---|
| 1080p | 3.132 | 3.053 | **+2.58%** | 2.42/2.72/2.61 |
| 4K | 11.765 | 11.454 | **+2.72%** | 2.56/2.72/2.86 |

对照 R1 收尾同口径（+1.58%/+2.70%）：4K 持平；1080p 比例上升系 LLPC 绝对值随机器状态同步变快
（R1 3.266→本轮 3.053，ACO 3.327→3.132），跨会话绝对值漂移，比例以同会话为准。
本夜零晋级，GREEN_HEAD 保持 `c75740e`。


## 11. Stage 4 R3：PAL 元数据对齐与 MIMG clause 束宽（两项判负，机制均按预测命中）

GREEN_HEAD 保持 `c75740e`。硬件口径：gfx1201 wave32 VGPR 文件 1536、分配粒度 8 —— 88 与 96 VGPR
同为满占 16 waves/SIMD，本轮无占用档位变化。

### 11.1 R3-E1 GFX12 PAL 容器元数据失配修复 — 判负（零收益）

根因确认并修复三处：`.image_op`（旧 splice 从不写，模板 clear_range 为 false，导致 21 个含 MIMG 的
核在 PAL 元数据里与 LLPC 相反）→ `r.set0>=0`；`.mem_ordered`（GFX12 RSRC1 无该位，旧代码把模板的
true 覆写成 false）→ is_gfx12 保持 true；`.sgpr_count` 108→106（模板 `.sgpr_limit` 106）。

**全仓核实**：109 条记录中 `set0>=0` 恰 **21** 条，与含 MIMG 指令的记录 **21/21 一一对应（0 例外）**。
**真实路径取证**：splice 后按既有 `ONR_ACO_DUMP` 导出 `.spliced.elf`，短跑导出 94 条逐条解码断言：
**94/94 `.mem_ordered=true`、`.sgpr_count=106`、`.image_op == (set0>=0)`（16 true / 78 false）**；
代表核 post_f_lt image_op=true / attr=true / enc_stage_64_64_96=false。两侧 runtime 分别构建
（base 958b1da5…、cand f2d4ba5c…；注意 mingw PE 时间戳使重建 SHA 非确定，SHA 不作语义证据）。

门禁：units 全绿；base-vs-cand 四相位 **exact 100.000000% / ulp 0 / psnr 999**。
测速：首轮 600 帧 @idx 似 4K −0.76%（BDA 核 −0.72%、纹理核 −0.94%，各核比例一致），**交换两侧复跑
后总计 −0.02%** → 首轮为测量顺序/会话偏置；3×ABBA（互换）4K **+0.353%**（组 −0.105/+0.353/+0.924）、
1080p **+0.215%**（组 +0.278/−0.186/+0.215）。
裁决：**PAL 元数据失配不是 ACO-vs-LLPC 差距来源**；元数据对齐对运行时间无影响。归档
`s4-r3-item1-pal-meta/`（candidate.patch、dump/ 94 条 spliced ELF、dump.tsv、verify_splice 源码、
stamps、swap 对照、ABBA 原始 JSON）。

**合入决定（Step 0）**：尽管该修复对帧耗时 0% 中性，但它是 GFX12 PAL 3.6 容器规范与越界修正
（`.sgpr_count` 108 超过模板 `.sgpr_limit 106`、`.image_op` 与 `.mem_ordered` 与 LLPC 容器语义相反），
0-ULP 与 3×ABBA 证据已在 `s4-r3-item1-pal-meta/` 取全 —— 现作为后续 FP8 量化轮的新干净基线
`GREEN_HEAD` 合入（仅 `gpu/src/rr_pal_binary.hpp` 三处，不含临时 dump 钩子）。

### 11.2 R3-E2 post_f_lt 5-tap 凑满 `s_clause 0x4` — 判负（clause 形态对齐，收益噪声级）

Mesa 26.3.0 `aco_scheduler.cpp::schedule_VMEM` 增 `ONR_ACO_MIMG_CLAUSE`：`current->isMIMG()` 时
`clause_max_grab_dist` 10 → 64，使首个 `image_sample_lz` 跨 ~23 条坐标计算下移并入后 4 条。
补丁经 prepare_capture 组合式叠加、`aco_scheduler.cpp` 入 patched 还原清单 + FATAL marker 自检；
`build_aco_records.py --mimg-clause-shaders` 白名单（仅 post_f_lt）。

上机前预检（全命中）：manifest **仅 post_f_lt code 变**（95b8b8e1 → **b96b1028**，其余 108 条
逐字节不变）；反汇编 `image_sample_lz` 仍 20、clause 由 `10×0x1/7×0x2/6×0x3` 变为
`10×0x1/7×0x2/**4×0x4**/2×0x3`（4 个 bicubic 分支各成 5 连发，与 LLPC 同形）；vgpr 96 不变、
scratch 0、exec 12216→12156。

门禁：units 全绿；四相位 0-ULP；日志同 spv（d57398b3）异 code = 真 A/B。
测速（600 帧、扣漂移、奇数槽 0.0000 ms CLEAN）：4K `@40` **−2.21 μs（−0.17%）**、
1080p **−2.19 μs（−0.67%）**；单组 4K ABBA 组中位 **−0.160%**（call0 −0.399%、call1 +0.079%）。
裁决：形态完全对齐 LLPC 但核内仅 −2.2 μs（<5 μs 噪声带），按门禁判负归档 `s4-r3-item2-post-clause/`。

**收敛结论（post 差距四重证伪）**：等待条数（R1-E2 wait0）、LDS 载入/合并（R2-E1）、k 循环展开
（overnight item1）、采样 clause 束宽（本轮）均无效 —— post 的 14.5% 差距只剩 SALU/VALU 排布与
整体调度，属编译器侧，源码/记录层无可达杠杆。还原：Mesa marker=0、hook=2，记录与 GREEN_HEAD 零差异。
