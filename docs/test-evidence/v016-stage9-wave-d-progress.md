# OpenRR Stage 9 (Wave D) 执行进展（封板归档）

工作树 `codex/mesa-263-comparison`（代码与构建终点 **`67d5ab2`**，前序 Round 3 提交 `be5b1ce` / `4d7c0ae`，父封板 `b26472a` = tag `stage8-seal-b26472a`）。最终门禁总表见 [`v016-stage9-wave-d-final-audit.md`](v016-stage9-wave-d-final-audit.md)。

## 0. 冻结与基线

- tag `stage8-seal-b26472a` 已打；`frozen-stage8-seal/` 与 `pre-unshackle-9ebfb19/` 作历史对比锚。
- 隔离评价准则：严禁将交付包 `build/aco-v015/stage/` 或 `final-pkg/stage/` 直接传给跑分/单元测试可执行文件，全部 GPU 测试在 `eval-work/stage/` 与独立临时副本运行，确保交付包始终保持 `296/296` SHA-256 一致且零 `.log` 污染。

## 1. WP-D1 — `MRG8_W` 偏移修复 + `enc_64`/`enc_96` `LT_MRG8=1`（晋级保留，含 Round 4 勘误与隔离）

1. **缺陷修复与重开**（`onr_block_w2.glsl` / `fastmode.cmake`）：修复 `ENC_CIN == 0` 的 UT8 尾段在 `16*Co*C`（`test_asi_units` 新增 UT8/U8 回读断言通过），为 `enc_stage_64_64_96` 与 `enc_stage_96_128_128` 启用 `LT_MRG8=1`。
2. **Round 1 `p1-abba-4k` 方向勘误**：按实际加载身份复核 `p1-abba-4k` 与 `p1-abba-1080p` 日志，`p1-candidate` 相对 `frozen-stage8-seal` 实为 **4K 均值 `−55.4 µs`（6/6 胜，最好腿 `−104.3 µs`；本文件原误记为最差 `+104.3 µs`）、1080p 均值 `−6.5 µs`（5/6 胜）**。
3. **Round 4 `enc_96` 独立隔离验证（`d1a-enc96-nomrg8-*`）**：在终版基础上单独撤掉 `enc_stage_96_128_128` 的 `LT_MRG8=1` 作为候选对照：
   - `@idx 4K`：`enc_stage_96_128_128_f_lt` 由 `441.5 µs` 退化至 **`459.0 µs`（`+17.5 µs`）**；
   - 无探针 `3×ABBA`：`4K` `mean = +3.8 µs, med = +4.7 µs`（`2/6` 胜），`1080p` `mean = +6.8 µs, med = +9.9 µs`（`1/6` 胜）。确认 `enc_96` 单开 `LT_MRG8=1` 收益为正，维持合入。
4. **WP-D1b（`enc_160 MRG8_UNROLL`）**：实测 `216 VGPR`、`40,448 B LDS` 触发资源上限，维持禁用归档。

## 2. WP-D2a / D2b — WGP、`shared-cmat` 与 `LT_POST_STILL`（全部无探针闭环归档）

1. **`shared-cmat=load` A/A 勘误 + `loadstore` 实测**：复核证实 `--shared-cmat load` 生成的两个 DFN 核 `.nrp` 与对照同 SHA-256（原"160 条标量已向量化零收益"系误判，实为真 A/A，现撤回该表述）。Round 4 对 `dec_dfn_160_256_{fl,f}_lt` 实测 `--shared-cmat loadstore`（捕获产生新哈希 `e79bbfb8318163c7` / `1c8b3af526051861`）：`@idx 4K −0.2 µs`（`98.2 -> 98.0 µs`），无探针 `4K 3×ABBA` **`mean = +13.5 µs, med = +5.9 µs`（`1/6` 胜）** → 判负归档。
2. **DFN WGP 无探针复测（`d2a-wgp-abba-4k`）**：补齐 Round 2 缺失的无探针仲裁，4K `3×ABBA` `mean = −1.2 µs, med = −2.7 µs`（`3/6` 胜，完全处于噪声带且内含自身 `+9.5 µs` 退化）→ 维持 CU 归档。
3. **Post WGP 无探针复测（`d2b-post-wgp-abba-{4k-confirm,1080p}`）**：`4K` 干净 `3×ABBA` `mean = +20.2 µs, med = +7.9 µs`（`2/6` 胜；首发 `20:06` 批次因外部 GPU 短测触发 `g2c0 +10.914 ms` 毛刺已重跑替换）；`1080p` 中位 `−2.5 µs`（`4/6` 胜，含 `g0c0 +783.6 µs` 离群）→ 无稳定增益归档。
4. **`LT_POST_STILL` 作用域说明**：源码 `gpu/shaders/include/onr_post_main.glsl:130-139` 使用 `subgroupAll(ix == 0 && iy == 0)` 检查整条 wave 是否全部落在纹素中心；合成分支运动评测 `+10.3 µs`，无静帧命中收益归档。

## 3. WP-D3 — `W2_96UP=1` 平反晋级 & `C=128 W2` 编译/数值证伪关闭

1. **`W2_96UP=1` 无探针全面平反并晋级（`gpu/fastmode.cmake:21`）**：Round 2 单次有探针负判（`dec_dfn_96 +21.6 µs`）被 4 套无探针 `3×ABBA` 实测彻底推翻：
   - **`4K New=1`**（`d3-w296up-abba-4k`）：`10.6104 -> 10.5369 ms`，**`mean = −73.5 µs (−0.693%), med = −73.9 µs, 5/6 胜`**；
   - **`1080p New=1`**（`d3-w296up-abba-1080p`）：`2.7992 -> 2.7875 ms`，**`mean = −11.7 µs (−0.418%), med = −12.1 µs, 6/6 胜`**；
   - **`4K Fast=2`**（`d3-w296up-abba-fast2-4k`）：`10.1596 -> 10.1228 ms`，**`mean = −36.7 µs (−0.361%), med = −38.6 µs, 6/6 胜`**；
   - **`1080p Fast=2`**（`d3-w296up-abba-fast2-1080p`）：`2.6348 -> 2.6255 ms`，**`mean = −9.3 µs (−0.353%), med = −8.6 µs, 6/6 胜`**；
   - **合计 `23/24` 条腿胜出**！`@idx 4K`（`FLOW=0`，`d3-w296up-idx-4k-stamps`）中 `dec_up_96_128_128_f_lt` **`459.9 -> 416.2 µs`（`−43.7 µs`）**、下游 `dec_dfn_96_128_f_lt` **`383.4 -> 381.8 µs`（`−1.6 µs`）**，22 核和 **`−46.1 µs`**；Anchor #2 **`140/140` 帧 `output_sha256` 逐字节完全一致**，ACO 规格 `256 VGPR / 12,288 B LDS / 0 scratch`。
2. **`C=128 W2` 硬证伪关闭**：`dec_up_128_128_160 W2=1` 直接命中 `onr_block_w2.glsl:96` `#error`；`dec_dfn_128_128 W2=1` 因 `onr_block_w2.glsl:26-34` 上限 `NT <= 6` 丢失通道 `96..127`，Anchor #2 PSNR 暴跌至 **`10.2814 dB`**，仅因此跑少半通道才快 `−310.5 µs` → 永久关闭。

## 4. WP-D5 — `LT_W1A8` 勘误与无探针 `3×ABBA` 双分辨率终结

1. 第一轮裸 `llvm-mc-21` 因 GFX12 `0xd769` 误码全部作废；修复 5 处 Bug 后 ACO 原生后端实测 `fp8=260/236/204`、`cvt_pk_fp8=496/416/384`，但实际寄存器达 **`216 / 216 / 240 VGPR`**，**均超出续跑单 `<= 192` 目标**（不得笼统写作"ISA 全绿"）。
2. `test_asi_units` 所报 `+297 µs` 对照的是 `New=1`（`k_lite / _lt`），非 `Fast=2`。
3. 画质 `Q_S0_1080p_dlaa` 较 `stage8-seal` 降 **`−0.2910 dB`**、较同基 `d1a`（`52.4559 dB`）降 **`−0.3270 dB`**；Round 4 补跑无探针 `3×ABBA` 双分辨率均 `0/6` 负判：**`4K` `+316.8 µs (+2.97%)`**、`1080p` **`+73.9 µs (+2.65%)`** → 彻底归档。

## 5. WP-D6 — `LT_PV8=1` (`dec_dfn_128_128`) 原型实测与晋级

1. **撤回无效外推**：撤回原将 `W1A8 −0.291 dB` 直接外推为 13 核固定损失及 `<180 µs` 未实测理论净上限的表述。
2. **`LT_PV8=1` 实现要点**（`gpu/shaders/include/onr_block_fast.glsl:403-411, 979-1015, 1068-1099` + `gpu/fastmode.cmake:20` + GFX11 剥离）：
   - `QV_T8_PV` 从 `yn` 零 shuffle 拷贝至 `ya_` 并 `coopMatLoad(wb_, E4View(wb8).v, ..., ENC_C, COL)` 保持 `vector8` `global_load_dwordx2`（`161 -> 161`，`0` 条 `global_load_u8`）；
   - `pv8_pack` 用 `qe4_pair32` 将 `V_h` 压入 `lds2`（`ds_write_b16` `64 -> 0` 转为 4 条 `ds_write2_b64`，`ds_read_b64` `129 -> 97`）；
   - Softmax 归一化后计算 `float r256_ = float(RCP_SUM(sum)) * 256.0`（将 `P·256 ∈ [0, 256]` 塞入 e4m3Fn 正规域 `[0, 448]`），`PV8_LOAD_A` + `coopMatMulAdd` 累计 `QC8` 后 `* (1.0 / 256.0)` 反缩放回 `o0, o1`。
3. **实测全绿晋级**：
   - ACO ISA（`dec_dfn_128_128_f_lt`）：`192 VGPR, 108 SGPR, 9,728 B LDS, 0 scratch`；`v_wmma_f32_16x16x16_fp8_fp8` **`160 -> 192 (+32)`**、`v_wmma_f16_16x16x16_f16` **`136 -> 104 (−32)`**、`v_cvt_pk_fp8_f32` `72 -> 168 (+96)`、`v_cvt_f16_f32` `260 -> 196 (−64)`；
   - Anchor #2（vs `d3-w296up`）：`140/140` 有限（`total_non_finite = 0`），7 个实测参考用例最差仅 **`−0.0195 dB`**（`Q_S1_4k_qual 47.7750 -> 47.7555`），`Q_S2_4k_perf` 反升 `+0.0504 dB`、`P00` `+0.0298 dB`；
   - `@idx 4K`：`_f_lt`（`FLOW=0`）`179.4 -> 175.2 µs`（`−4.2 µs`）、`_fl_lt`（`FLOW=1`）`146.8 -> 123.0 µs`（**`−23.8 µs`**）；`3×ABBA 4K` **`mean = −9.3 µs (−0.087%), med = −45.1 µs, wins = 4/6`**，`1080p` `mean = −0.9 µs` → 晋级保留。

## 6. Round 4 评测器、数值安全与原子构建硬化

1. **Anchor #2 评测器修复**（`runtime/bench/openrr_contrast_eval.cpp`）：全部回读帧独立扫描 `NaN/Inf` 并落盘汇总 `total_non_finite`；缺参考帧记 `missing_ref` 且无参考用例 `min_psnr01_db` 输出 `null`（消除 `999 dB`）；支持 `output_sha256`、`--dump-frames` 与 `--compare-frames`（生成 `frame_compare.csv`，实证 `FLOW=0` 与 `FLOW=1` 全 **`140/140` 帧 `max_ulp=0`**）。
2. **`Item NFMA` 抓虫与 `downsample*_lt` 修复**：实测发现 `native` 编译把 `onr_common.glsl:38` `half_rz()` 里的 `abs(float(h)) > abs(x)` 优化抹除，导致输入 `>= 65504` 时溢成 `+Inf`；将 4 支 `downsample*_lt` 迁回 `global` 后 65504 与 `half_rz` 格点测试全绿，而保留 `37` 支 `_lt`/`_lx` 核在 `native` 下所有 `16` 条 `v_pk_fma_f16` 融合完好（同时使 `Fast=2` 单帧 `P00` 回到 `global` 的 `47.6201 dB`，并将 `Q_S2_1080p_qual f15/31/63` 提高 `+0.97..+1.10 dB`）。
3. **原子构建与洁净打包**：`build_aco_records.py` 在任何捕获失败时写入 `failures.json` 并先清理现场 `manifest.json`/`manifest.msgpack`；`build_shaders.py` 自动清理过期 `.spv`；`build_asi.sh` 新增 `OPENRR_CLEAN_STAGE=1` 并清理目录 `.log`；`test_aco_build_policy.py` **`11/11` 全 PASS**。
4. **最终洁净复验与全仓提交**：`build/aco-v015/stage/` 与 `final-pkg/stage/` 于所有门禁跑完后复查仍维持 `296/296` SHA-256 一致与 `0` `.log`；代码提交于 `codex/mesa-263-comparison` **`67d5ab2`**，全部性能/质量更新详见 [`v016-stage9-wave-d-final-audit.md`](v016-stage9-wave-d-final-audit.md)。
