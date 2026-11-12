# Stage 2 第一刀：TR=1 深层核 256-VGPR（5 waves）墙攻坚——负结果与定位（`3b23cef` 之上的实验，已回退）

分支 `codex/mesa-263-comparison`。本报告为**纯无损尝试**的完整实验记录：两项源码级手术（XIN8 延迟解码、OUT_T 有界乒乓）均未撼动 ACO 的 256-VGPR 分配，按门禁（任一档变慢即回退 / 噪声带无实质收益即回退）全部回退；工作树与产物已核验恢复至 `3b23cef` 逐字节等价态。附三刀 PROF 诊断定位与下一步路线修正。

## 1. Step 1 — 基线（`3b23cef` Fast-ACO，全部 C≥96 深核）

| 内核 | VGPR | waves/SIMD | exec(B) | scratch |
|---|---:|---:|---:|---:|
| dec_dfn_128_128_f_lt / fl_lt | **256** | **5** | 25536 / 26708 | 0 |
| dec_dfn_96_128_f_lt / fl_lt | **256** | **5** | 23256 / 22272 | 0 |
| dec_up_128_128_160_f_lt / fl_lt | **256** | **5** | 24148 / 25524 | 0 |
| enc_stage_96_128_128_f_lt / fl_lt | **256** / 240 | 5 / 6 | 29284 / 36072 | 0 |
| enc_stage_128_128_160_f_lt / fl_lt | 240 | 6 | 32688 / 31972 | 0 |
| 160 家族（enc160_160/0、dec_up_160、dec_dfn_160） | 192 | 7 | 20312–28668 | 0 |

（waves 按 24-VGPR 粒度分配：>240→5，≤240→6，≤216→7；全部 scratch=0。）

## 2. Step 2 — XIN8 延迟解码（方案 2b，兼评 2a）

实施：FP8QV 下 `XIN8` 只在 head 循环前构造 `QB8 y0..y9`（OCP B-tiles）；`MH x0..x9` 延迟到 `CUT(3)` 之后、`RESID_T` 首读点之前从只读 `pc.x` 重读解码（XOUT 尚未执行、无别名写；铁律遵守——x 从原始字节解码，绝无从 y 反推）。2a（驻留 rw 字）被 2a 的同一失败机理支配（见下），未单独实施。

结果（全部 C≥96 核）：**VGPR 逐核零变化**（256/240/192 全持平）。
机理：ACO 的调度器不尊重源级生命周期——pc.x 的 buffer_reference 加载与 e4m3 解码是无副作用的数据流，ACO 为隐藏延迟把它们**重新提升回 head 循环区**，源码上的"x 生命周期已死"不构成寄存器分配约束。CUT() 在生产构建是空操作，无调度屏障可用。

## 3. Step 3 — OUT_T 有界乒乓预取（SPLIT==1，NT==8）

实施：仿 `onr_block_w2.glsl:324-339`，`SPLIT==1 && NT==8` 分支改为先备 `b0l/b0h`（o0）与 `b1l/b1h`（o1）双 B 操作数（8 VGPR），wout 权重经 `(p0,p1)/(q0,q1)` 双 tile 乒乓窗（16 VGPR）按 `n=0..7` 推进；每累加器严格先 o0 半块后 o1 半块，乘加序与原实现逐位一致。NT≠8 的核走原路径。

结果：**VGPR 依旧零变化**；代码体积略降（dec_dfn_128_fl 26708→26564）。

**组合（2b+3）运行时核验**：
- 数值：Fast-ACO 新 vs `3b23cef` Fast-ACO：steady/DRS **exact=100.0000%、ulp=0**（0 ULP 铁律满足——乒乓/延迟解码确实保序）；
- 速度（3 轮 32/120/64 ABBA 中位）：1080p **+0.12%**、4K **+0.27%**（噪声带、无实质收益）→ **按门禁回退**。

## 4. 诊断定位 — 三刀 PROF 切除（dec_dfn_128_128 f/fl_lt，静态）

| 切除 | VGPR | 结论 |
|---|---:|---|
| `PROF_NOUP`（跳 U 乘积） | 与基线同哈希（dec_dfn 无 DEC_UP，无操作） | — |
| `PROF_NOGW`（WSTEP 权重改 LDS 存根，切断全部全局权重预取面） | **256** | 权重预取并发不是峰值源 |
| `PROF_NOEXP`（exp 近似直通） | **256** | attention 超越函数路径不是峰值源 |

三项独立切除全部停在 256 → **压力峰是 TR 体常驻工作集的聚合**：NT=8 个 f32 累加器（64 VGPR）+ x0..x9（32-40）+ y0..y9（16-20）+ QV 临时（q0/q1/v0/v1，16）+ attention 流动临时（qh0/qh1、e2、pc2 等）+ MLP 切片并存期。它不归属任何单一可切除/可重排区段，源码级"错峰生命周期"手术对 ACO 无效（调度器重排回填）。

## 5. 下一步路线修正

1. **ACO 侧旋钮**（不动 GLSL）：Mesa 26.3 的 `max_waves`/occupancy 目标、`schedule_pressure` 类编译策略（`aco_statistics` 的周期模型正是为此）——对指定 `.nrp` 重捕即可逐核 A/B，这是当前性价比最高的未试杠杆。
2. **tile 数削减**（动 GLSL、保 0 ULP）：把 head 循环拆成两趟、每趟只持 4 个 c 累加器（每累加器乘加序不变，输入重读一次）。改动面大、预取换寄存器的净收益未知，列为候选。
3. cmat-address 有界寻址扩展（前报告既定路线）仍独立有效——它砍的是 ds_load_b64/v_perm 寄生指令而非 VGPR。

## 6. 复现

```bash
# 基线/实验 VGPR 对照：build_asi.sh 捕获日志逐核行，或
python -c "import json; m=json.load(open('build/asi-015-aco/stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1/manifest.json')); [print(r['spv'], r['vgprs'], r['exec_bytes']) for r in m['records'].values()]"
# 诊断变体：glslang -DENC_F32=1 [-DFLOW=1 -DFLOW_SLOT=7 -DFLOW_WAIT=1] -DLT_EXP=1 -DLT_RCP=1 -DLT_HM=1 -DPROF_NOGW … + build_aco_records.py
# 速度/数值门禁：build/v015-runs/stage2/speed.ps1 / openrr_bench parity 轮（正文 §2-3 口径）
```

## 7. 勘误（`f02597d` 后交叉核验）——256 VGPR 纹丝不动的真正根因是 WGP LDS 物理上限

§5.1 的"ACO occupancy 旋钮"路线**无效**，特此勘误。RDNA4 每 WGP 共享 **128 KiB（131,072 B）LDS**（4×SIMD32），workgroup 级占用率上限为：

```
waves/SIMD = floor(131072 / shared_bytes) × (workgroup_size / 32) / 4
```

（manifest 交叉核验，`_fl_lt` 变体）：

| 内核 | wg | LDS(B) | **LDS 物理上限** | VGPR 档 | 实际 |
|---|---:|---:|---:|---:|---:|
| dec_up_128_128_160 | 128 | 32768 | **4 waves** | 256（5w） | 4 |
| dec_dfn_128_128 | 128 | 26112 | **5 waves** | 256（5w） | 5 |
| enc_stage_96_128_128 | 128 | 19456 | **6 waves** | 240（6w） | 6 |
| enc_stage_128_128_160 | 128 | 21504 | **6 waves** | 240（6w） | 6 |
| dec_dfn_96_128 | 64 | 9728 | **6 waves** | 256（5w） | 5（VGPR 卡） |
| 160 家族 | 256 | 30208 | **8 waves** | 192（8w） | 8 |

ACO 先按 LDS 上限钳制 `num_waves`，再把 `max_reg_demand` 放宽到该档最大 VGPR（5w→256）——**寄存器没有降到台阶以下的动力，因为占用率已被 LDS 封顶**。除非同时削 LDS（改 tile 布局，动数值语义），VGPR 手术不可能改变这些核的占用率。160 家族已满 8 waves 仍慢 1.4–1.6×，进一步证实差距主体是**访存指令 mix**而非占用率。重心正式转向 RADV coopmat 访存降低（见 `v015-stage2-radv-cmat-results.md`）。
