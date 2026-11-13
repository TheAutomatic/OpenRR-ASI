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
160 族 FP8 量化噪声经 DLAA 路径（无升采样稀释）在 ~33 dB 时序累积段的反馈放大，与 Item 1
在 enc0 的重置段放大同构、幅度小一个量级。后续若收紧 DLAA 画质预算，重开面 = 160 族 FP8
的 dlaa 档守卫。

**2c 后 ACO vs LLPC 差距重测**（600 帧 @idx，修正配对，`s4-quality/stamps.json`）：
1080p +0.138 ms（3.175 vs 3.037）、4K +0.343 ms（11.687 vs 11.343）——2c 削 0.046/0.061 ms，
160 族跌出榜前列。新榜首：post_f_lt +0.164（1.145×）、d3d_enc0_attr +0.090（1.106×）、
长尾 1.02–1.05× 合计 ~0.09。

**B1.2 post BATCH（判负归档 `s4-b1/BATCH-REJECTED.md`）**：20 采样先发后混 0-ULP 成立
（四相位 exact 100.000000%/ulp 0），但 +3.31%/+2.96%——96→192 VGPR、总指令 2074→3309、
s_wait_loadcnt 反而 20→95。**B2.1 enc0_attr 预取（判负归档 `s4-b2/REJECTED.md`）**：
0-ULP 成立，装载真去重 29→18、等待 120→98、VGPR 持平，但 +0.21%/+1.80%——六组值跨极值
循环活距延长反噬。**两核同构结论：ACO 的逐装载交错调度在 VGPR 约束下已是局部最优，
RDNA4 上源级"先发后算"系统性负收益；post/attr 的剩余差距在后端编译器侧，不在着色器源排布。**
B1.1 WGP 轮判无效（A/A，`s4-b1/INVALID.md`），重做待按 spv 哈希选记录+日志确认。
