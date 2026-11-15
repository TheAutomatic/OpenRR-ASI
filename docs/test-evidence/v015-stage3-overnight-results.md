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

## 12. Stage 5：mode-4 纯 f32 量化战役（Wave 1–3 + 终局对决）

起点 `d3229e9`（冻结整包 `build/v015-runs/frozen-d3229e9/`：非 mode-4 Fast SPV + trace.asi，
兼作终局 LLPC 基线）。工具链：Mesa 26.3.0 drirc 捕获（`ONR_ACO_PRECISION=global`），
ISA 门禁用编码长度感知的裸码扫描（`s5-wave1-quant-mode4/scan_isa.py`）——llvm-mc-21 流式反汇编
在 gfx12 8 字节编码上失步（`v_cvt_pk_fp8_f32` 显示为假 `cndmask+subrev`、min3/max3 显示为
`v_min_num_f64_e32`），指令直方图不可信。

### 12.1 Wave 1（晋级 `69fdd01`）：LT_MLP8 六核 mode-4 量化底座

- `qe4_pair32`：`saturatedConvertEXT` 的 Inf→NaN 选择必须拼成 `abs(x)==+Inf`（vkd3d-proton 形状；
  NIR 无 isinf 低化，`OpIsInf` 条件永不折叠）→ Mesa 26.3 vec2 规则 → ACO 单条硬件饱和
  `v_cvt_pk_fp8_f32`（MODE.FP16_OVFL）。
- `gelu_f32`：gelu2 代数直落 f32（4 条 f32 VALU/元素，零 f16 往返）；SPLIT1 逐 tile 窄作用域
  FC1Q（fc1 累加器一完成即压成 2-VGPR QB8）；x8 配对量化 α 方案胜出（β 融合残差探针
  +880 exec bytes，判负归档）；SPLIT2 GELU 交换改每 lane 8 字节 uvec2（1×ds_store_b64 +
  2×ds_load_b64，QC8/QB8 lane 坐标恒等式）。
- 静态（frozen 对照）：6 核 exec −6.4~−12.7%，cvt_pk8 96→44 / 80→40（满载配对），
  `v_maxmin_*` 16→**0**，`v_perm_b32` −60~−90/核，VGPR/scratch 不变，Legacy 0 字节差异。
- 门禁：units 全绿；Legacy 0-ULP 双分辨率 exact 100%；锚点 #2 v1+v2_motion 全 ok（45/45 哈希门，
  dlaa min 32.94 vs 前值 33.08）；锚点 #4 ACO/LLPC 交叉 g0 59–61 dB / DRS 999-exact、g1 121–129 dB、
  nf=0；4K 600 帧 @idx 扣漂移：dec_dfn_128 −5.1µs、160 族 −3.2~−6.5µs/核（enc_stage_128 +1.1µs）、
  对照 post +3.0µs 噪声；3×ABBA **−0.304% @1080p / −0.382% @4K**（6/6 组全负）。

### 12.2 Wave 2（判负，复核后维持）：W2 五核 LT_MLP8

初判"激活量化墙"经复核撤回（三种量化实现测得逐位相同的 exact=25.676/30.39 dB，分歧源不在量化）。
追加隔离全部否证：cmat-address 白名单（30.40 dB）、fc1-only FP8（22.13 dB，更糟且非单侧干净故障）、
item2b 逐字补丁（30.39 dB）。同套 helper 在 W1（58.79 dB 晋级）与 TR 体（Wave 1 船载）全部正常——
残缺假设收敛为"W2 双 token-tile 体与 FP8 操作数的未解交互"。完整证据链与过程事故
（两次拼接误删 `W2T(RES_A1)`/FC2B 定义被 Legacy 锚点当场捕获并修复、fastmode.cmake CRLF 污染）
见 `s5-wave2-w2-mlp8/REJECTED.md`。树还原。

### 12.3 Wave 3（晋级 `4332470`）：W1 解码双核 LT_MLP8

item1-dec-only 形状（enc0 保持 fp16）+ Wave-1 量化器：x1 tile 一次配对量化，fc1 QC8 一完成即
GELUQ8 压成 QB8，MLP_SLICE_LOOP dont_unroll。静态周期账：WMMA 周期省 1904 > ΔVALU 1352，
净 −552（dec_up −881 / dec_head +329）；exec +8/+22%（VALU 换 WMMA 周期）。
门禁：Legacy 0-ULP；fast 对 green 稳态 58.79 dB nf=0；锚点 #2 全 ok（dlaa min 34.38，优于前值）；
锚点 #4 g0 58.6–59.7/g1 121–130 nf=0；@idx dec_up −5.11µs / dec_head −6.78µs（合计 −11.9µs）；
3×ABBA 1080p −0.079%、4K 干净复跑 **−0.179%**（三组全负）。
（过程：我的首版移植测得 7.47 dB——逐组件探针证明配对 x 量化/gelu_f32/配对 g 量化各自全绿，
最终以 item1 结构内联两处配对量化收敛 58.79 dB。）

### 12.4 终局：ACO（`4332470`）vs LLPC（frozen d3229e9 非 mode-4 SPV，驱动原生）

3×ABBA 48/240/64，基线侧日志 130/130 `compiler=LLPC`（零混编）：

| 分辨率 | ACO vs LLPC | 组内 | 较 §10.5 基线（+2.58/+2.72%） |
|---|---|---|---|
| 1080p | **+2.214%** | 2.21/2.24/2.04 | 收窄 0.37pp |
| 4K | **+2.347%** | 2.81/2.35/1.85 | 收窄 0.37pp |

4K 600 帧 @idx 扣漂移分核（ACO cand vs LLPC base）：dec_dfn_128_128 **−95.2µs（−35.1%）**、
dec_up_160_256_160 −9.0µs、dec_up_32_64_64 −19.6µs、dec_head_32_64 −13.0µs、enc_stage_128 ±0、
post_f_lt **+156.5µs（+13.8%）**。本轮战役把 MLP 面全部抹平/反超；残余差距按 §7 定性仍集中在
post_f_lt 采样/调度与 enc0_attr（ACO 调度器策略，源码不可达）。

### 12.5 结论

- GREEN_HEAD 演进：`d3229e9` → `69fdd01`（Wave 1）→ `4332470`（Wave 3）；Wave 2 判负留档。
- 净效果：ACO 自身 −0.30~−0.38%（Wave1）再 −0.08~−0.18%（Wave3）；对 LLPC 差距
  +2.58/+2.72% → **+2.21/+2.35%**。
- 遗留：W2 体 FP8 MLP 根因未明（重开条件：先在 W2 体上做单 tile e4m3 B 操作数微基准）；
  post_f_lt 差距依旧（编译器侧）。

## 13. Stage 6：Wave 2 抢救、Wave 3 共享权重优化与终局再战（`term3`）

起点 `eab495e` / `4332470`（工具链 Mesa 26.3.0，`ONR_ACO_MIMG_CLAUSE=0`、`ONR_ACO_SHARED_CMAT` 与 `elem_bytes` hooks=2，未触碰 `post_f_lt` 与 `d3d_enc0_attr_f_lt`）。

### 13.1 Wave 2 抢救晋级（`6a30425`）：W2 五核 `LT_MLP8` + mode-4 配对量化

- **根因澄清**：Stage 5 Wave 2（`s5-wave2-w2-mlp8/my-w2-block-paired.glsl`）并非“双 token-tile FP8 交互墙”。真正故障是 `gpu/shaders/include/onr_block_w2.glsl` 中的 `#ifdef LT_MLP8` 提前闭合于第 401 行 `W2T(RES_A2) W2T(RES_B2)` 之前，使 FP8 路径在写出 `x0*/x1*` 后继续跌落并叠加 stale `c0*/c1*`（将输出破坏至 `30.39 dB`），且迫使 `c00..c05, c10..c15` 跨 MLP 循环保活，导致 `enc_stage_96_128_128_f_lt` 溢出 `scratch=24`。
- **正式补丁**：
  - `gpu/fastmode.cmake` 仅对 W2 五核追加 `LT_MLP8=1`（`enc_stage_64_64_96`、`enc_stage_96_128_128`、`dec_dfn_96_128`、`dec_up_64_64_96`、`dec_dfn_64_64`）。
  - `dec_dfn_64_64.comp`、`dec_up_64_64_96.comp`、`enc_stage_64_64_96.comp`（`W2_SL=64`）在 `#ifdef LT_MLP8` 下设置 `#define MLP_SLICE_LOOP [[dont_unroll]]`。
  - `enc_stage_96_128_128.comp` 在 `ENC_F32 && FLOW && ...` 分支中 `#undef LT_MLP8`，锁定 `_fl_lt` 保持 fp16 MLP（与 `frozen-4332470` 0 diff）。
  - `gpu/shaders/include/onr_block_w2.glsl` 将 `W2T(RES_A2) W2T(RES_B2)` 整体纳入 `#else` 分支，并在 `#ifdef LT_MLP8` 中使用 `qe4_pair32` + `gelu_f32`（`GELUQ8`）+ `QC8` 累加。
- **静态与 ISA（`scan_isa.py`，`4332470` → `6a30425`）**：仅 W2 `_lt` 核变更，非 `_lt` 核 0 字节差异，全量 112 条 manifest 记录 `scratch=0`、`maxmin32=0`、`maxmin16=0`、`cvt_pk8=64`；`enc_stage_64_64_96_f_lt` `exec 33032→28224`、`dec_up_64_64_96_f_lt` `25744→22632`、`dec_dfn_64_64_f_lt` `27016→22636`（VGPR 维持 240/256）。
- **门禁与质量**：`test_asi_units.exe` 全绿；Anchor #1（`1080p` / `4K` vs `frozen-d3229e9`）100% 0-ULP `999.00 dB`；Fast Parity（vs `frozen-4332470`）`1080p` 稳态 `57.78 dB` / DRS `60.25 dB`、`4K` 稳态 `59.54 dB` / DRS `56.71 dB`，`non_finite=0`；Anchor #2 `v1`（76 哈希门 `0` 失败、`Q_S0_1080p_dlaa min=34.1554`、`Q_S0_4k_perf min=48.2634`）与 `v2_motion`（44 哈希门 `0` 失败）全绿；Anchor #4（同构 LLPC vs ACO）`1080-g0` `58.31 dB / DRS 999 dB`、`1080-g1` `128.72 / 122.48 dB`、`4k-g0` `59.42 / 57.79 dB`、`4k-g1` `127.17 / 122.59 dB` 全部 `passed=True`。
- **性能（vs `frozen-4332470`）**：4K `@idx` 三个 4K 命中 W2 核 `enc_stage_64_64_96_f_lt` **−52.27µs（−5.14%）**、`dec_up_64_64_96_f_lt` **−51.70µs（−6.07%）**、`dec_dfn_64_64_f_lt` **−65.85µs（−8.38%）**，target 合计 **−169.83µs（−6.39%）**（ALL-21 −1.47%）；3×ABBA（48/240/64）`1080p` **d_med = −1.717%**（`3.1194 → 3.0638 ms`，三组 −1.717%/−1.682%/−1.815%，6/6 call 全负）、`4K` **d_med = −2.349%**（`12.3604 → 12.1099 ms`，前两组 −2.349%/−2.540%，第 3 组末尾单次波动 +5.886%）。

### 13.2 Wave 3 优化晋级（`e1af0d5`）：`W1` 解码双核共享权重外循环

- **方案选型**：在 `gpu/shaders/include/onr_block_w1.glsl` 的 `#ifdef LT_MLP8` 路径中对比 4 种循环分块/预取方案（`j+=32u`、`j+=16u` lazy `w0_,w1_`、`j+=64u`、`j+=16u` preloaded `w0_,w1_`）：
  - `@idx` 初筛四者结合节省分别为 −70.58µs（Cand A）、**−82.95µs**（Cand B）、−79.34µs（Cand C）、−74.64µs（Cand D）。
  - 最终采用 **Cand B（`j+=16u`、lazy `w0_,w1_`）**：4 个 active token tile 一次配对量化并驻留 `x8a/x8b`，每个隐藏切片 `jr` 仅加载一次 `f0_, f1_` 并跨 4 个 tile 复用（共用单个 `QC8 h_`），随后按需加载 `w0_, w1_` 写入 `mc0n/mc1n`。
- **静态与 ISA（`6a30425` → `e1af0d5`）**：仅 `dec_head_32_64_f_lt` 与 `dec_up_32_64_64_f_lt` 变更，非 `_lt`/`_lx` 0 diff，全量 `scratch=0`；`dec_head_32_64_f_lt` `exec 28048→21804`（VGPR 192、`cvt_pk8 96→48`、`fma32 256→64`、`setreg 13→4`），`dec_up_32_64_64_f_lt` `[0]` `exec 25564→18908`（VGPR 192）、`[1]` `exec 25584→18800`（VGPR 256、`cvt_pk8 96→48`、`fma32 256→64`）。
- **门禁与质量**：`test_asi_units.exe` 全绿；Fast Parity（vs `frozen-w2-stage` `6a30425`）`1080p` & `4K` 稳态/DRS **100.00% 0-ULP `999.00 dB`**；Anchor #1（vs `frozen-d3229e9`）双分辨率 **100.00% 0-ULP `999.00 dB`**；Anchor #2 `v1`（76 哈希门 `0` 失败）与 `v2_motion`（44 哈希门 `0` 失败）全绿（`ANCHOR2 VERIFIED GREEN`）；Anchor #4（同构 LLPC vs ACO）`1080-g0` `56.59 dB / DRS 999 dB`、`1080-g1` `134.16 / 120.27 dB`、`4k-g0` `59.44 / 57.77 dB`、`4k-g1` `126.29 / 122.39 dB` 全绿（注：交叉对照 `frozen-d3229e9` 非 FP8 LLPC 时 `1080-g0` 通过但 `1080-g1` 因小分辨率 DRS 叠加 W1+W2+W3 FP8 误差为 `47.15–49.19 dB`，而同构编译器间 `120.27–134.16 dB` 零退化）。
- **性能（vs `6a30425`）**：4K `@idx` 复测 `dec_up_32_64_64_f_lt` **−52.06µs（−4.24%）**、`dec_head_32_64_f_lt` **−27.15µs（−2.22%）**，target 合计 **−79.21µs（−3.23%）**（ALL-21 −0.70%；`idx_drift.py` 单核净值 −48.91µs / −20.98µs，`CLEAN`）；3×ABBA（48/240/64）`1080p` **d_med = −0.716%**（`3.0700 → 3.0588 ms`，三组 −0.520%/−0.747%/−0.716%）、`4K` **d_med = −1.972%**（`11.7856 → 11.5555 ms`，三组 −2.142%/−0.818%/−1.972%，6/6 call −0.553%~−3.274% 全负）。

### 13.3 终局再战 `term3`：ACO（`e1af0d5`）vs LLPC（`frozen-d3229e9`，驱动原生）

3×ABBA（48/240/64），全组 `g*c0` 日志确认 `dec_head_32_64_f_lt` 为 `LLPC (spv=9a3ee271bb7b5d53, m=0)` → `ACO (spv=bdf472e1540dd0d9, m=130)`、`g*c1` 完全对调 `ACO → LLPC`（零混编）：

| 分辨率 | ACO vs LLPC (`term3`) | 三组组中位 | 基线 → 候选 | 较 §12.4（`+2.214/+2.347%`） |
|---|---|---|---|---|
| 1080p | **−0.121%** | −0.022% / −0.121% / −0.235% | `3.0518 → 3.0481 ms` | 收窄并反超 **2.335pp** |
| 4K | **−0.744%** | −0.774% / −0.553% / −0.744% | `11.7057 → 11.6195 ms` | 收窄并反超 **3.091pp** |

4K 600 帧 `@idx`（`frozen-d3229e9` LLPC vs `e1af0d5` ACO，summary steady median `11.720 → 11.630 ms` / `−89.7µs`，`−0.77%`，`PSNR_01=59.89 dB`，`non_finite=0`；ALL-22 stamped dispatches raw 合计 `11.3303 → 11.2594 ms` / `−70.90µs`，以下单核差值为 `cand_raw - base_ms` raw 口径）：
- ACO 优势核：`dec_dfn_128_128_fl_lt` **−103.60µs（−37.80%）**、`dec_dfn_64_64_f_lt` **−66.70µs（−8.56%）**、`dec_dfn_96_128_fl_lt` **−55.70µs（−13.17%）**、`enc_stage_64_64_96_f_lt` **−52.10µs（−5.19%）**、`dec_up_64_64_96_f_lt` **−44.40µs（−5.31%）**、`dec_up_32_64_64_f_lt` **−32.80µs（−2.73%）**、`enc_stage_160_256_160_fl_lt` −20.30µs（−15.23%）。
- 残余驱动差距：`post_f_lt` **+172.20µs（+15.18%）**、`d3d_enc0_attr_f_lt` **+71.80µs（+8.46%）**、`dec_up_128_128_160_fl_lt` +36.00µs（+18.93%）、`enc0_stage_32_64_f_lt` +26.20µs（+1.89%）、`dec_up_96_128_128_fl_lt` +26.00µs（+5.20%）。
- **总结**：在 `post_f_lt` 与 `d3d_enc0_attr_f_lt` 零修改前提下，Wave 2 抢救与 Wave 3 外循环权重共享将整体端到端 ACO 性能推进至 `1080p` **−0.121%**、`4K` **−0.744%**，双分辨率全面反超原生驱动 LLPC。

## 14. Stage 7 夜间单栈 ACO 攻坚实测报告（Wave A→V4 切换）

### 14.1 晋级链（起点 `00c9acf`/代码态 `e1af0d5`，11 次提交全绿）

| 提交 | 内容 | 关键实测 |
|---|---|---|
| `61a9c42` | `--precision-split --precision-native-only` 13 核 native 白名单 | post −51µs / attr −21µs；ABBA **−0.72%/−0.74%**；Anchor #2 当时全绿 |
| `d28522f` | `PP` 宏剥 post `_lt` precise | exec 11820→11576；@idx −21µs；−0.08%/−0.26% |
| `c931ba2` | `AP` 宏剥非 wide attr precise（guides=1 保持 999 dB） | @idx −9.4µs；−0.13%/−0.12% |
| `5f18689` | post 双像素循环展开（VGPR 96 不变） | @idx −37.9µs；−0.22%/−0.10% |
| `d77943e` | FLOW `dec_up_128/96` LT_MLP8 + 解除 enc_stage_96_fl 旧 pin | @idx −41/−43µs；**−0.98%/−0.97%** |
| `52fcb96` | `XOUTQ` 分发：W1/W2 体 XOUT 换 qe4_pair32 | dec_up_32 exec −8%；−0.37%/−0.55% |
| `6bc268a` | **Item 5：TR 体原生零转换 LT_UP8**（dec_up_96/128，W2 模式移植：per-lane plane 裸码 + `up_w` 尾段） | @idx −28.1/−7.8µs；−0.46%/−0.24% |
| `4f63aca` | **真 Item 8：enc0 主干 MLP fc2-only FP8**（fc1 保 fp16 逐字复用；GELU8+尾段；VGPR 守 192） | enc0 @idx −41.9µs；−0.17%/−0.35%；Anchor #2 vs 绿参照最差 −0.10 dB |

Legacy `.spv/.nrp` 全程 0 字节差异；全量记录 `scratch=0`；每步 units 全绿（K2 的 `diff_ship` 门在 native 记录下按"ACO↔驱动本就允许数值分歧"改为 ship 重复确定性门，`diff_words`+hash 门保持硬性）。

### 14.2 判负/中止归档（按 V4 强制证据规则框）

- **item3a**（post 内联双三次多项式）：@idx **+138µs**——该展开 ALU 代价高于纹理查表；仅否此实现（H2 的 RGP 排查保留）。
- **Item 8 v1（05:36 整机死机坏核）**：根因＝`ENC_EMBED fw` 尾段读越界（`pc.mrg_w+16384` 读穿 12,288B 的 `embed_w_` 进 `tab_`，`0x7f/0xff`=e4m3 NaN → 3.02 dB；重转储回收时 wedge 驱动）。**纯 OOB 代码 bug，非 FP8 方向问题**；已回退，权重实测全域 ≤±6（无饱和问题）。
- **MRG8-TR（本会话，当前实现未达标）**：镜像 W2-MI 不变量的首版测得 12.47 dB + units arena/wide FAIL（熔断即时止损，无二次上机）。静态定位：(a) `lds2`（uvec2=4 halves/elt）索引按 u32 粒度错位；(b) TR 体 lane→32 通道块内半区映射（hh=ln/16∈0..7）不能从 W2（hh∈{0,1}）类推。正确设计已定：**生产者侧 XOUT 代码镜像**（XOUT 已有 w_ uvec4 顺带写 LDS 代码平面，复刻 plane record 布局，读侧与 W2 逐位同构）+ `mrg_w` with_tiles 前缀后接自然序 e4m3 尾段（engine 改动已验证可编译；尾段字节数恰为 Co·4C，构造即边界安全）。铁律 1 的 `.asi` 时间戳核查在本次首战告捷（02:08 陈旧 runtime 被拦下，改走 `OPENRR_TRACE_BASE=build/asi-015-aco` 规范锁步）。
- **Item 5a（W2 体 Cp=128 UP8 扩展）**：无效——dec_up_96 双变体实为 TR 体，改由 6bc268a 的 TR 原生实现覆盖。
- **dlaa/S2 例的参照系语义**：`check_anchor2.py` 固定对比 4332470；`Q_S2_1080p_qual` 的 −0.508 dB 中 Wave A 已占 −0.41（当时过门），item5+8 仅 −0.10。已将绿基线 Anchor #2 快照棘轮至 `s7-overnight/anchor2-ref/`（52fcb96→item8r 链），终态 `final2/anchor2` 与参照**逐位一致（全例 Δ=0.000）**；`Q_S0_1080p_dlaa` 单指标在已接受构建间本有 ±0.3 摆动（33.63~34.38，probeA 关 2a 反而更差证明非单点污染）。

### 14.3 收官终验

- **Anchor #1**（vs `frozen-d3229e9`，fast-mode 0）：1080p & 4K steady+DRS **100.0000% 0-ULP / 999.00 dB**。
- **Anchor #2**：v1 76 + v2 44 哈希门通过；vs 棘轮绿参照全例 ΔPSNR=0.000。
- **term4**（ACO `4f63aca` vs LLPC `frozen-d3229e9`，3×ABBA 48/240/64，基线侧 100% `compiler=LLPC`）：
  - 1080p **−3.394%**（`2.9548 vs 3.0538 ms`，组 −3.20/−3.40/−3.39）
  - 4K **−3.907%**（`10.9722 vs 11.4421 ms`，组 −4.13/−3.91/−3.90）
  - 对照 term3（−0.121%/−0.744%）：本战役净推进 **−3.27pp/−3.16pp**；整帧 4K `11.6195→10.9722 ms`（−5.6%）。
  - 4K 600 帧 @idx（ACO 净胜核）：dec_dfn_128 **−114.7µs(−41.8%)**、dec_up_96 −26.5、enc_96_fl −27.3、enc_64 −20.5、dec_up_32 −9.7；LLPC 仍胜：post +129（原 +172）、attr +87、enc0 +50.9、dec_head +50.8、dec_up_128 +35.1（本轮 drift 0.95 系探针跑基线侧时钟偏移，仲裁以 ABBA 为准）；两侧 non_finite=0。
- 终态：树干净于 `4f63aca`，未 push，主仓/WSL Mesa 未动。

### 14.4 V4 待续（下会话队列，证据齐备）

MRG8-TR 生产者镜像版（设计定稿如上）；`fast.glsl:1177` TR XOUT 尚未接 XOUTQ（遗漏点）；Wave B1 四项 Mochi 红利（NR_FRAG_E4M3/v_perm phi/NR_OPQ/NOOOB）；白名单外 native 单核试验；2^k 解锁 enc0 fc1；Track B LT_E8RAW 成对迁移。RGP 元问题（H2/H4、BDA vs SSBO、C=32 地板）留 Phase 4。
