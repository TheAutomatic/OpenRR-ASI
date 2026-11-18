# v0.1.5 Stage 2 日间轮报告（2026-11-12 日，承接过夜轮）

分支 `codex/v015-stage2-overnight`（过夜轮基线 `88a0f35`）。测速口径：中间 3×32/120/64、最终 3×48/240/64 配对交替中位；主目录只读；未推送。
过夜轮 §2.4 留裁决项已由用户裁定通过（g1 锚点 29.69 dB = 修复前 LLPC-Fast 的原轨迹，见 v015-integration-results.md 表 A 第 176 行）。

## 0. 提交链（本轮新增）

| 提交 | 内容 |
|---|---|
| `703271f` | **Step 1**：`attr_log2`/`attr_exp2` IEEE 边界守卫 |
| `ac42b98` | **Step 2**：`LT_ROLL_HD/J/J2/UP` 数控展开钩子（实测全负向，全部默认关闭） |
| （本提交） | 本报告 |

`fastmode.cmake` 与深层核记录相对过夜轮 `c8ca717` 后状态净零（Step 2 全回退，唯一保留产物为 wide attr 核的守卫版记录）。

## 1. Step 1 —— IEEE 边界加固 ✅（`703271f`）

- `attr_log2`：`isnan(x) || x < 0 → NaN`（原实现把 NaN 的指数域抹成有限尾数算出 ~128.6，击穿 `signed_log` 的 isnan 清洗把坏像素饱和到 +8）；`±0 → -inf`；`+inf → +inf`；次正规钳到最小正规数。
- `attr_exp2`：NaN 透传（保 `decode_hist` 下游 `no_nan()` 语义）；`x ≥ 128 → +inf`（指数进位污染符号位）；`x < -126 → 0`（次正规编码洞）。
- 专项验证：位级精确仿真 harness（精确有理 fma + 正确舍入 fp32，无二次舍入）——NaN/±Inf/±0/次正规/极值 HDR 边界 + 2×20000 随机点，log2 绝对误差 ≤2.1e-4、exp2 相对 ≤7.3e-6，**0 失败**（`build/v015-runs/stage2/attr_edge_check.py`）。
- GPU 门禁：g1 ACO-vs-LLPC 126.76/126.81 dB @1080p/4K（与守卫前逐位同——守卫在合法输入上是死代码）；速度 -0.13%/+0.05%（零退化）。

## 2. Step 2 —— 深层核展开抑制：实测全面负向，机制结论修正 ❌（`ac42b98`）

钩子（数字宏受控，规避 CMake bracket 语法坑）：`HD_UNROLL`（fast 模板两处 + w2 head 循环）、`MLP_SLICE_LOOP`（w2 两处 MLP j 循环）、`J2_UNROLL`（SPLIT==2 隐层循环）、`UP_UNROLL`（DEC_UP tile 循环）。

| 实验 | 静态效果 | 运行时效果 |
|---|---|---|
| `LT_ROLL_HD=1` ×10 深核 | 5 核码体积 −18~27%（C=128 三核 + enc/dec_up 96）；VGPR 全部不变（256/240 顽固）；**160 家族 + dec_dfn_96 记录逐字节不变**（ACO 无视其 DontUnroll） | DEEP10 +0.7% @1080p / **+1.9% @4K**（dec_up_96 1.03×→1.15×） |
| `LT_ROLL_J=1`（W2 MLP） | enc_96 −20%、dec_up_96 −25% | enc_96 串行 1.33→1.27×，dec_up_96 **+25%**；端到端 +1.3~2.5% 三档全慢 |
| 仅留 enc_96 ROLL_J（择优后） | — | 端到端 **+1.32/+2.53/+1.63% 全档变慢**（串行微利被 flow 并发的回边预取失效吞掉）→ 全回退 |

**机制结论修正**（写入 2.0B 后续路线）：
1. ACO 的 head/MLP 展开是**承重的**（跨迭代预取）；滚动它必然变慢——LLPC 的滚动代码快是因为它的调度器在滚动态仍能预取。
2. **静态码大小与运行时无关**：dec_dfn_128 码 −27% 耗时不变。
3. 160 家族与 dec_dfn_96 的 DontUnroll 被 ACO 完全无视（字节恒等）——它们的 1.4–1.6× 与展开无关。
4. 剩余差距驱动 = **指令 mix**（每核 s_wait_alu +148、ds_load_b64 +129、v_perm +113 的寄生开销）→ 下一步应主攻 LDS/地址下降低（如把 cmat-address 有界寻址补丁扩到全部深层核——目前仅 dec_dfn_96_128_fl_lt 与 enc_stage_128_128_160_fl_lt 两核启用）。
5. post_f_lt/d3d_enc0_attr 快查按"无便宜收益不恋战"跳过（同机制结论下展开旋钮无望）。

## 3. Step 3 —— 2B/2A-2：未施工（映射手术超本轮预算），前置与防坑已归档

- **2B（ENC_MERGE FP8）**：三条实现路径全部评估——
  ① A-tile 寄存器构造（仿 `XIN8`/`REC2Y` 的字节→coopmat 装填）：需逆向 merge 的 arena 片段 lane↔tile 映射（MST 的 `a_` 编址 `(t>>4)*4 + (t&7)>>1` 行选择 + `pos*ENC_C` 列内 token 编织），静态推导风险高、错位只能靠画质门禁兜底；
  ② LDS e4m3 别名视图（`shared LdsE { floate4m3_t ... }`）：现有 LdsH/W/D/Q 四视图同尺寸（2·LDS_A 字节）依赖编译器合并，新类型异尺寸不合并 → LDS 翻倍风险；
  ③ 混合精度 B 侧 FP8（A=f16 × B=e4m3）：参考实现（mochizuki coopmm）仅匹配 e4m3×e4m3，VK coopmat 要求 A/B 同组件类型，封死。
  已确认的防坑事实：`e4m3x4` 产出 sm_86 编码、`OCP4()` 是精确转码路径（E8RAW 的 59.24 dB 教训）、mrg_w 的 E4M3 码已由 `U8(b.mrg_w)` 在 enc1 上传（offset 2·4·C·Co）。
- **2A-2（C≥96 DEC_UP FP8）**：同一 lane↔tile 映射 blocker（generic 体 U 循环的 A-tile 构造）；engine 侧 `U8(b.up_w)` 放开是一行，但 `qv_bytes` 对 C≥96 up_w 的 e4m3 精确性未验证。

## 4. Step 4 —— 收尾回归与三对比测速 ✅

### 4.1 正确性（当前最终形态 = `703271f` + 钩子基础设施）

| 项 | 结果 |
|---|---|
| test_asi_units | ALL PASSED（含 wide/DOF 序列、DRS/reset） |
| Legacy vs 冻结 v0.1.5-ACO | steady/DRS exact 100.0000%，ulp 0 |
| g0 ACO vs LLPC | 59.52 / DRS 70.55（与过夜轮同值） |
| g1 ACO vs LLPC | **126.76** / DRS 121.73，nf 0 |
| 奇数 2562×1441（g1） | 122.03 / DRS 123.65 |

### 4.2 最终 3×48/240/64

对比①（当前 vs 冻结 `v0.1.5-ACO`）：

| 档 | 当前 | 冻结 | Δ |
|---|---:|---:|---:|
| 1080p | 3.362 | 3.369 | **-0.22%** |
| 1440p | 5.654 | 5.658 | **-0.08%** |
| 4K | 12.287 | 12.320 | **-0.26%** |

→ 本轮全部改动（守卫 + 钩子）合计零代价、微正向。

对比②（当前 ACO strict vs 同版 Fast-LLPC）与对比③（vs 原版门禁线 3.009/11.389）：

| 档 | ACO strict | 同版 LLPC | 差距 | 距门禁线 |
|---|---:|---:|---:|---:|
| 1080p | 3.358 | 3.008 | +11.61% | +0.349 ms |
| 1440p | 5.662 | 5.143 | +10.08% | — |
| 4K | 12.321 | 11.378 | +8.29% | +0.932 ms |

本次 LLPC 3.008/11.378 与 §9.1 基线（3.009/11.389）一致到 0.1%——GLSL 改动对两编译器共同提速为零（预期：守卫/钩子不改合法输入语义）。**门禁未跨**（差 >0.5% 临界带，无需补 5 轮）：深层核差距主体在指令 mix，等待 2.0B 的 LDS/地址降低路线（§2 结论 4）。

## 5. 结论与后续路线

1. 本轮净增：IEEE 加固（安全）+ 展开钩子基础设施（未来实验用）+ 2.0B 机制修正（快杠杆证伪、主攻方向改 LDS/地址）。
2. 深层核追平 LLPC 的下一步（原推论，已被下方 §6 勘误修正）：
   a. **cmat-address 有界寻址扩展**：现仅 2 核启用（dec_dfn_96_128_fl_lt、enc_stage_128_128_160_fl_lt，构建时 `--cmat-address-shaders` 列表），扩到全部深层核并按核 A/B（针对 ds_load_b64/v_perm 寄生）；
   b. 2B/2A-2 的 lane↔tile 映射手术（需专门时段，本报告 §3 的三条路径评估可直接复用）；
   c. ACO 上游对比（26.3.0 vs 更新的 ACO 调度器是否已修这些形状）。
3. 复现命令沿用 `build/v015-runs/stage2/` 脚本（speed/final_speed/stamps_run/single/attr_edge_check/isa_hist）。

## 6. 源码复核勘误与路线修正（Post-Review Errata）

合入 `codex/mesa-263-comparison` 后，对照 Mesa 补丁与 shader 源码逐行复核，修正上文 §2.4 / §3 / §5.2 中的三处机制误判（§1–§4 的实测数据与门禁结论全部维持不变）：

1. **修正 §2 结论 4 / §5.2a（`cmat-address` 适用条件误判）**：
   `asi/build/mesa/global-address.patch`（`ONR_ACO_CMAT_ADDRESS`）仅匹配 `nir_var_mem_global` + **`GL_CooperativeMatrixLayoutColumnMajor`** + FP16，完全不触碰 LDS（`ds_load_b64`）或 `ROW_MAJOR`。而 `TR=1` 深层核（`dec_dfn_128` 等，`gpu/shaders/include/onr_block_fast.glsl` `WSTEP`/`W8STEP`）权重采用 **`ROW_MAJOR`**，因此单纯把深层核加入 `--cmat-address-shaders` 对 `dec_dfn_128` 几乎无效。真正需要排查的是 RADV `radv_nir_lower_cooperative_matrix.c` 对 `ROW_MAJOR` global load 与 `shared` LDS `coopMatLoad` 的向量化/标量化路径。
2. **补齐 §2（`C=128` 顽固顶满 `256 VGPR` → 5 waves 占用率的源码根因）**：
   `gpu/shaders/include/onr_block_fast.glsl`（约 L696–L731）在进入 `for (uint hd = 0u; hd < NH; ++hd)` 注意力循环**之前**就声明并驻留了 `ACC c0..c7`（8 个 `float32` coopmat 累加器 = **64 VGPR**）+ `MH x0..x7`（32 VGPR）+ `QB8 y0..y7`（16 VGPR）= **112 VGPR 保底常驻**，叠加循环内 `s_`/`v_` 必然顶穿 256 VGPR 上限。下一步应参考 `gpu/shaders/include/onr_block_w2.glsl` 的 `W2_DEFER=1` 思路，将 `TR=1` 的 `out` 投影累加器 `c0..c7` 推迟到 `hd` 循环之后（注意力阶段仅把 `y_h` 暂存回空闲 LDS 后再分段/统一累加 `out`），直接释放注意力热点期的 64 VGPR。
3. **修正 §3（`2B ENC_MERGE` 与 `2A-2 C≥96 DEC_UP` 的伪“LDS 翻倍 + sm_86 编码”阻断）**：
   - **零字节 LDS 膨胀**：`gpu/shaders/include/onr_block.glsl`（L386–L399）启用了 `GL_EXT_shared_memory_block`，所有无实例名 `shared Block { ... }` 均在偏移 `0` 共址重叠（总 LDS 取 `max` 而非求和）；新增 `shared LdsE { floate4m3_t lds8[2u * LDS_A]; };` 恰好是 `2 * LDS_A` 字节，与 `lds2` 等长，**LDS 增量为 0 字节**。
   - **权重已是标准 OCP E4M3**：`gpu/src/engine.cpp` 的 `qv_bytes()` 上传到 `b.mrg_w` 尾部的本就是标准 **OCP E4M3**（仅运行期 arena 动态激活 `e4m3x4` 是 `sm_86` 编码，已由 `OCP4()` 精确转码）。因此无需逆向 arena `lane↔tile` 寄存器映射：只需把转码后的 8 个 OCP E4M3 激活打包写入 `lds2`，再经 `coopMatLoad(a8_, lds8, ..., gl_CooperativeMatrixLayoutRowMajor)` 装填 A-tile，即可同时打通 **`2B`（`ENC_MERGE` FP8 WMMA）** 与 **`2A-2`（`C≥96 DEC_UP` FP8 WMMA）**。

