# OpenRR Stage 9 (Wave D) 执行进展（进行中）

工作树 `codex/mesa-263-comparison`（父锚点 `b26472a`，本地 tag `stage8-seal-b26472a`；冻结对照包 `build/v015-runs/stage9/frozen-stage8-seal/`，58 MB）。本文随轮次更新；终审见 Phase 3 的 `v016-stage9-wave-d-final-audit.md`。

## 0. 冻结与基线

- tag `stage8-seal-b26472a` 已打；`frozen-stage8-seal/` 含 `stage/` + `openrr-trace.asi`。
- 画质对照锚：`build/v015-runs/stage8/final-remed-anchor2/`（= 封包包的 v1 76 / v2 44 结果）。

## 1. WP-D1 — MRG8_W 偏移修复 + enc_64 / enc_96 重开 LT_MRG8（晋级）

1. **缺陷修复**（`gpu/shaders/include/onr_block_w2.glsl`）：`MRG8_W` 原无条件按 U8 尾段 `2*(Co*4C + C*4*Cin)` 取址；`ENC_CIN == 0` 的 enc2+（UT8 打包）真实尾段在 `16*Co*C`。该 bug 此前已被 `enc_stage_128_128_160`（已开 LT_MRG8、CIN=0）实际踩中——本轮修复同时纠正了它。fast 体（`onr_block_fast.glsl` MRG_W8）原本就正确。
2. **重开**（`gpu/fastmode.cmake`）：`LT_DEFS_enc_stage_64_64_96` / `LT_DEFS_enc_stage_96_128_128` 各加 `LT_MRG8=1`；gfx11 剥离表（CMakeLists / build_shaders.py）已含 LT_MRG8，零 FP8 影响。
3. **资源档位**（manifest，vs seal）：enc_64_f_lt VGPR 240→240、LDS 不变；enc_96_f_lt 256→256；enc_96_fl_lt 240→**256**、LDS +6 KB（FLOW 镜像）；均 `scratch=0`。
4. **单测**：`test_asi_units` 新增 WP-D1 断言——enc1 U8 尾段 `2*(Co*4C+C*4*Cin)` 与 enc2..enc4 UT8 尾段 `16*Co*C` 精确命中 `mrg_w` e4m3 码、fp16 前缀逐字节一致（D3D12 打包为地址子片，经 `fixed_` blob 解析回读；`-0.0` 的符号位差异按数值零等价处理）。全 PASS。
5. **Anchor #2**（frozen `d1a-mrg8-pkg`，fast-aco，vs seal `final-remed-anchor2`）：v1 全用例 `+0.0000 .. +0.0377 dB`；v2_motion 最差 `−0.0980 dB`（Q_S2_4k_perf 41.1592），未触 −0.25 dB 单步警戒、远高于固定账本 −0.70 dB 硬线；non_finite=0。
6. **快速计时**（test_asi_units 引擎档，24 帧短测）：4K Fast p50 **10.944 ms**（seal 档 11.096，−152 µs），1080p 2.852（seal 2.847，噪声带内）。@idx 4K 配对与 3×ABBA 待终包跑。


### 1.1 @idx 4K 配对（p1-candidate vs frozen-stage8-seal，600 帧，FLOW=1，drift 带内）

| 核 | seal µs | cand µs | Δ |
|---|---|---|---|
| enc_stage_64_64_96_f_lt（MRG8 开，U8 尾段） | 898.1 | 860.2 | **−37.9（−4.2%）** |
| enc_stage_96_128_128_fl_lt（MRG8 开，UT8 修复尾段） | 520.3 | 519.3 | −1.0 |
| 22 核总和 | 10575.0 | 10541.9 | −33.1 |
| 稳态中位（summary.json） | 10.6464 ms | 10.5898 ms | **−56.6 µs（−0.53%）** |

未改核 ±0.0…+2.7 µs（≤0.36%，带内）。勘误：seal 中 `enc_stage_128_128_160` 虽带 `LT_MRG8=1`，但 C=128 走 TR 体（`onr_block_fast.glsl` MRG_W8 本就正确），W2 体的偏移 bug 从未实例化——属潜伏缺陷，本轮由 enc_96（C=96 W2 体）首次启用时修复并验证。

### 1.2 3×ABBA（p1-candidate vs frozen-stage8-seal，FLOW=1，锁步 .asi+data-dir）

| 档 | 中位差 | 均值差 | 最差腿 | 噪声带（±0.23/0.26%） |
|---|---|---|---|---|
| 4K Qual | −4.9 µs | +2.9 µs | +104.3 µs（单腿毛刺） | ±24 µs |
| 1080p Qual | +2.9 µs | +0.7 µs | 10.0 µs | ±7.4 µs |


整帧 ABBA 在噪声带内（核级 −33 µs ≈ 0.3%，处于 ABBA 分辨率边缘）；核级配对（drift 校正）与稳态中位（−56.6 µs）一致向好，画质全绿 → **D1a（enc_64/enc_96 MRG8 + 偏移修复）晋级**。

## 2. WP-D1b — enc_160 MRG8_UNROLL（归档回退）

`MRG8_UNROLL`（`onr_block_fast.glsl` q 循环 `[[dont_unroll]]`）+ enc_160_256_160 `LT_MRG8=1` 实测：VGPR 192→**216**（目标 ≤192 未守住）、LDS 30208→**40448**（+10 KB）。按计划归档回退：宏保留（ inactive），fastmode 已还原。代码字节 −492 但驻留双破门槛。

## 3. WP-D4 — `_f_lt` native 白名单补齐（合入）

`asi/build/build_asi.sh` `--precision-native-only` 补入 `dec_up_128_128_160_f_lt / dec_dfn_160_256_f_lt / enc_stage_160_256_0_f_lt / enc_stage_160_256_160_f_lt` 四个实机默认 `ONR_RR_FLOW=0` 变体。manifest 验证 4 核 `precision=native`，VGPR/LDS/scratch 不变（仅 code 字节变化）。

### 3.1 FLOW=0 vs FLOW=1 正面对决（p1-candidate，4K Qual，600 帧，STAMPS）

| 模式 | 稳态中位 | p95 | 22 核和 | 核间空隙和 |
|---|---|---|---|---|
| `ONR_RR_FLOW=1`（跑分口径） | 10.6674 ms | 11.1366 | 10536.8 µs | 175.0 µs |
| `ONR_RR_FLOW=0`（D3D12 宿主默认） | **10.6120 ms** | **10.9453** | 10561.7 µs | **66.4 µs** |

FLOW=0 净赢 ~55 µs（空隙 −108.6 µs 吸收 barriers 串行 +24.9 µs；32→34 持久核尾波饥饿气泡消除，与计划预判一致）。终审按裁决边界双列两套权威水位（默认实机态 FLOW=0 / 流控态 FLOW=1），不混标；不改宿主默认（`runtime.cpp:924` 维持 FLOW=0）。

## 4. WP-D5 — LT_W1A8 原生 e4m3 全 FP8 头（第二轮：ISA 平反，双门禁判负归档）

### 4.1 第一轮 llvm-mc 结论作废（勘误）

第一轮"全包零 `v_cvt_pk_fp8_f32`、`v_cndmask` 暴涨 735/851"的结论 **100% 错误**：Ubuntu llvm-mc-21 不识别 GFX12 `v_cvt_pk_fp8_f32` 的 VOP3 首字 `0xd769xxxx`，warning 后错位 4 字节，把操作数高字节 `0x02` 误判为 VOP2 `v_cndmask_b32_e32`。本轮起所有 ISA 统计改用 ACO 原生后端（`RADV_DEBUG=shaders`，截取 `After lowering to hw instructions:`），并可用 `.nrp` 机器码 `(dword >> 16) == 0xd769` 交叉核验。

### 4.2 第二轮：修复 5 处着色器 Bug 后重测

指令单定位的 5 处 Bug 全部修复：①量化/转置循环上限 16→8（wave32 每 lane 8 元素，消除指令翻倍）；②Q/V 补齐 K tile n=1（通道 16..31，此前整半输入漏乘）；③`W1A_OW` 补 `hd * 32u` 列偏移（此前 hd=1 错乘 hd=0 的 W_out）；④`W1A_EDIR` 行列转正（A=key 转置 × B=own）+ `W1A_ELO` 还原 fp16 精确恒等阵（严禁未归一化 exp 进 e4m3）；⑤Q 逐 tile 窄作用域（同时仅 2 个 QC8）+ V 直存 `QA8 vt`（量化即转置）+ `W1A_OKT` 恢复 `IFK` 稀疏剪枝。

**ISA 门禁（ACO 原生后端，全部达标）**：

| 核 | fp8 WMMA | f16 WMMA | v_cvt_pk_fp8_f32 | v_maxmin | v_cndmask | VGPR | scratch |
|---|---|---|---|---|---|---|---|
| enc0_stage_32_64_f_lt | 260 | 14 | 496 | 0 | 17 | 216 | 0 |
| dec_up_32_64_64_f_lt | 236 | 10 | 416 | 0 | 4 | 216 | 0 |
| dec_head_32_64_f_lt | 204 | 10 | 384 | 0 | 18 | 240 | 0 |

硬件单指令量化转换器全程在场；f16 WMMA 仅剩 LT_SYM 的 E_LO 恒等阵。

### 4.3 双门禁实测（判负归档）

- **性能**：units 引擎档 4K Fast 11.075 ms（p1-candidate 10.778，+297 µs/+2.8%）；1080p 2.903（+55 µs）。全秩 32 维 Q/V/O/W_out 乘法量显著高于被替换的 rank-16 `LT_Q16` + 免投影 `LT_VO` 近似，8 周期 FP8 折抵后仍净负，叠加 216/240 VGPR（8→7 waves/SIMD）。
- **画质（Anchor #2 fast-aco vs seal）**：v1 最差 **−0.2910 dB**（Q_S0_1080p_dlaa，52.1289）、−0.2818（Q_S0_4k_perf）——破单步 −0.25 dB 警戒线；v2 最差 −0.1857（Q_S2_4k_perf）。原因：FP8 通路把激活（x、softmax 概率）压入 e4m3（相对误差 ~4–6%），其代价超过卸除 LT_Q16/LT_VO 的 2.6–2.95% 权重截断收益。
- **裁决**：代码保留（`LT_W1A8` 未定义零影响），fastmode 还原 `LT_Q16/LT_VO`。重启前置条件：对激活做 2^k 逐 tile 缩放量化（DLSSNR-AMD 的 act-scale 方案）或等 toolchain 侧激活感知量化，否则 C=32 的 FP8 注意力在画质-性能双轴均不可赢。

## 5. 其他已落地（待终包测量）

- **WP-D2a**：`build_aco_records.py` 新增 `--shared-cmat-shaders`（逐核限定 `ONR_ACO_SHARED_CMAT` 作用域；指纹含 `shared_cmat_shaders`；记录含 per-record `shared_cmat`）；`test_aco_build_policy.py` 扩到 8 测全绿（作用域隔离、缓存指纹、off+名单报错、全局模式兼容）。
- **WP-D3 前置**：`onr_block_w2.glsl` LT_UP8 路径支持 `ENC_CP == 128`（`w3` 记录 + `UP_K3_8`），`>128` 拒绝；`W2_96UP` 门已存在，启用留待测量。
- **WP-D6 前置**：`XIN2Y`（w2 体）拆为 fma 段 + 集中量化段（压 `s_setreg` MODE 翻转）；`XQ8Y` 审视为纯量化无需拆。

## 7. 第二轮 Step 2–4 结果（全部按五项证据归档）

- **WP-D2a 实验 A（dec_dfn_160_256 WGP，`--wgp-mode-shaders`，rsrc1 bit29=1 验证）**：帧内配对 `dec_dfn_160_256_fl_lt` **86.8 → 96.3 µs（+9.5/+11%）**，下游 `dec_up_128_128_160_fl_lt` +6.2 µs；Phase-1 的单核 −10.1 µs 为孤立测量，帧内 WGP 反效 → 回退。
- **WP-D2a 实验 B（`--shared-cmat load` 逐核）**：`dec_dfn_160_256_fl_lt` **±0.0 µs**（LDS 访存非瓶颈，160 条标量 `ds_load_u16_d16*` 向量化零收益）→ 不保留（中性即复杂度无益），`loadstore` 不再测。
- **WP-D2b 实验 A（post_f_lt/post_dlt_f_lt WGP）**：`post_f_lt` 1215.9 → 1217.1 µs（+1.2，噪声），22 核和 +12.3 µs（带内偏负）→ 归档。
- **WP-D2b 实验 B（`LT_POST_STILL=1`）**：`post_f_lt` 1236.7 → **1247.0 µs（+10.3）**——合成评测全程运动，无静态区可赢，divergent `subgroupAll` 分支纯增开销 → 按硬门禁回退。
- **WP-D3（`W2_96UP=1`，dec_up_96_128_128 切 2-wave W2 体）**：资源门禁全过（VGPR 256、LDS 12 KB、scratch 0、workgroup 64）；自身 **431.6 → 421.6 µs（−10.0/−2.3%）** 但下游 `dec_dfn_96_128_fl_lt` **+21.6 µs（+6.1%）**（W2 体输出布局耦合）——结队净 **+11.6 µs** → 按结队准则回退。C=128 静态清点：C=96 双 tile 已占满 256 VGPR，C=128（NT=8）双 token tile 需 ~220+ 基础 + 双倍 Q/V/Wout 片段 → 必破 256 触发 spill，按计划不做盲跑，账本归档。
- **WP-D6.0（XIN2Y 跨 tile 拆平）**：落地为 `XINF`/`XINQ` 两相宏（`W2P(XIN_FMA_*)` 先全量 fma、`W2P(XIN_QY_*)` 后集中量化）；ACO 原生后端实测 `dec_up_64_64_96_f_lt` `s_setreg` **仍为 28**（源码两相间无屏障，调度器按寄存器压力自由重排，MODE 翻转不可源码级消除），配对计时全核 ±1 µs（中性）→ 保留代码形态（零代价、结构更清晰）。
- **WP-D6a/D6b（C≥64 的 13 核 S/O/W_out FP8+FP32）静态清点归档**：W2 C=64 现状 240 VGPR；FP8 化需新增 pb QB8（+8）、oh/量化临时（+16）、S/O QC8 累加器对（+16）→ 264+ 必破 256。且机制性证据：本轮 W1A8 实测同一算子模板（FP8 Q/V/S/O + e4m3 激活量化）画质 **−0.29 dB**（破 −0.25 警戒线）、性能 −2.8%；C≥64 的现行 fp16 注意力是**精确路径**（无 LT_Q16/LT_VO 近似可抵消），FP8 化为纯增量化误差 + 驻留降档，双轴可预见皆负 → 按"不做无效投入"条款整组归档，不做逐宽度盲跑。

## 8. 第二轮 Step 5 终审（完成，详见 `v016-stage9-wave-d-final-audit.md`）

- Anchor #1：4K + 1080p steady+DRS `max_ulp_diff = 0`（100.0000% 0-ULP）。
- Anchor #2 三列：fast-aco v1 全 ≥0（best +0.0307）/ v2 最差 −0.1415；exp-aco vs exp 基线 v1 最差 0.0000（best +0.0406）/ v2 最差 −0.1217；FLOW=0 与 FLOW=1 逐值一致；全部 `non_finite=0`。
- 6 档 × 3 模式水位（默认 FLOW=0）：Fast 4K Qual **10.4988 ms（3.60x 5090）**、1080p Qual 2.7706（3.18x）、1440p Qual 4.7235（3.36x）、4K Perf 6.6800（3.41x）、1080p DLAA 5.4492（3.53x）、4K UltraPerf 3.8916（3.02x）。
- 无探针 FLOW 终裁：4K `FLOW=1` 快 −49.1 µs（3×交错中位）——**第一轮 STAMPS 口径的 "F0 快 55 µs" 系探针串行化扭曲**；1080p 中性。宿主默认维持 FLOW=0，双列不混标。

## 9. 构建/门禁记录

- 全部构建：WSL `build_asi.sh build/aco-v015` + `build_runtime_trace.sh`；94/94 记录、0 failures、`scratch=0` 每轮核验。
- 套件：`test_aco_build_policy`（8/8）、`test_present_scope`、`test_dxgi_queue`、`test_asi_units`（含新 WP-D1/D5 断言 + 引擎 1080p/4K 档）全部 PASS。
- 事故 1：anchor2 首跑指向正被 W1A8 重建覆写的 `build/aco-v015` → "missing metadata" 假失败；改为冻结包 `-Pkg` 后通过（教训：测量一律用冻结目录）。
