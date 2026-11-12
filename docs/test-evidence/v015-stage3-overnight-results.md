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
