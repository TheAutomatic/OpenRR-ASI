# OpenRR Stage 9 (Wave D) 执行进展（Round 7 闭环归档）

工作树 `codex/mesa-263-comparison`（Round 7 代码终点 **`d7b5e0a`**；前序 Round 6 `79322a0` / `66f26b2`、Round 5 `f6dcc83` / `4cb973c`、Round 4 `2bbe662` / `67d5ab2`、Round 3 `4d7c0ae` / `be5b1ce`、父封板 `b26472a` = tag `stage8-seal-b26472a`）。最终门禁与 7 维总表见 [`v016-stage9-wave-d-final-audit.md`](v016-stage9-wave-d-final-audit.md)。

## 0. 冻结与基线

- tag `stage8-seal-b26472a` 已打；`frozen-stage8-seal/` 与 `pre-unshackle-9ebfb19/` 作历史对比锚。
- 隔离评价准则：严禁将交付包 `build/aco-v015/stage/` 或 `final-pkg/stage/` 直接传给跑分/单元测试可执行文件，全部 GPU 测试在 `eval-work/` 与独立临时副本运行，确保交付包始终保持 `296/296` SHA-256 一致且零 `.log` 污染。

## 1. WP-D1 — `MRG8_W` 偏移修复 + `enc_64`/`enc_96` `LT_MRG8=1`（晋级保留，含 Round 4 勘误与隔离）

1. **缺陷修复与重开**（`onr_block_w2.glsl` / `fastmode.cmake`）：修复 `ENC_CIN == 0` 的 UT8 尾段在 `16*Co*C`（`test_asi_units` 新增 UT8/U8 回读断言通过），为 `enc_stage_64_64_96` 与 `enc_stage_96_128_128` 启用 `LT_MRG8=1`。
2. **Round 1 `p1-abba-4k` 方向勘误**：按实际加载身份复核 `p1-abba-4k` 与 `p1-abba-1080p` 日志，`p1-candidate` 相对 `frozen-stage8-seal` 实为 **4K 均值 `−55.4 µs`（6/6 胜，最好腿 `−104.3 µs`；本文件原误记为最差 `+104.3 µs`）、1080p 均值 `−6.5 µs`（5/6 胜）**。
3. **Round 4 `enc_96` 独立隔离验证（`d1a-enc96-nomrg8-*`）**：在终版基础上单独撤掉 `enc_stage_96_128_128` 的 `LT_MRG8=1` 作为候选对照：`@idx 4K` `enc_stage_96_128_128_f_lt` `441.5 -> 459.0 µs (+17.5 µs)`；无探针 `3×ABBA` `4K +3.8 µs (2/6)`、`1080p +6.8 µs (1/6)`，确认单开 `LT_MRG8=1` 收益为正。
4. **WP-D1b（`enc_160 MRG8_UNROLL`）**：实测 `216 VGPR`、`40,448 B LDS` 触发资源上限，维持禁用归档。

## 2. WP-D2a / D2b — WGP、`shared-cmat` 与 `LT_POST_STILL`（全部无探针闭环归档）

1. **`shared-cmat=load` A/A 勘误 + `loadstore` 实测**：证实 `--shared-cmat load` 为真 A/A；Round 4 对 `dec_dfn_160_256_{fl,f}_lt` 实测 `--shared-cmat loadstore`（`e79bbfb8318163c7` / `1c8b3af526051861`）：`@idx 4K −0.2 µs`（`98.2 -> 98.0 µs`），无探针 `4K 3×ABBA` **`+13.5 µs (1/6)`** → 归档。
2. **DFN WGP 无探针复测（`d2a-wgp-abba-4k`）**：4K `3×ABBA` `mean = −1.2 µs, med = −2.7 µs (3/6)` 噪声内 → 维持 CU 归档。
3. **Post WGP 无探针复测（`d2b-post-wgp-abba-{4k-confirm,1080p}`）**：`4K` clean `3×ABBA` `mean = +20.2 µs, med = +7.9 µs (2/6)`；`1080p` 中位 `−2.5 µs`（`4/6` 含 `+783.6 µs` 离群）→ 归档。
4. **`LT_POST_STILL` 作用域说明**：源码 `gpu/shaders/include/onr_post_main.glsl:130-139` 使用 `subgroupAll(ix == 0 && iy == 0)` 检查整条 wave；合成运动评测 `+10.3 µs` → 归档。

## 3. WP-D3 — `W2_96UP=1` 平反晋级（Round 5 限定 `gfx12`）& `C=128 W2` 关闭

1. **`W2_96UP=1` 无探针全面平反并晋级（`gpu/fastmode.cmake:21`）**：Round 2 单次有探针负判（`dec_dfn_96 +21.6 µs`）被 4 套无探针 `3×ABBA` 彻底推翻：
   - **`4K New=1`**（`d3-w296up-abba-4k`）：`10.6104 -> 10.5369 ms`，**`−73.5 µs (−0.693%), 5/6`**；
   - **`1080p New=1`**（`d3-w296up-abba-1080p`）：`2.7992 -> 2.7875 ms`，**`−11.7 µs (−0.418%), 6/6`**；
   - **`4K Fast=2`**（`d3-w296up-abba-fast2-4k`）：`10.1596 -> 10.1228 ms`，**`−36.7 µs (−0.361%), 6/6`**；
   - **`1080p Fast=2`**（`d3-w296up-abba-fast2-1080p`）：`2.6348 -> 2.6255 ms`，**`−9.3 µs (−0.353%), 6/6`**；
   - **合计 `23/24` 腿胜出**！Round 5 按 `[P2]` 审查在 `asi/build/build_shaders.py:71-72` 与 `gpu/CMakeLists.txt` 显式剥离 `gfx11` 的 `W2_96UP=1`，使 `shaders-gfx11/dec_up_96_128_128_{f,fl}_lt.spv` 保持 `[128, 1, 1]`，仅 `gfx12` 启用 `[64, 1, 1]`。
2. **`C=128 W2` 硬证伪关闭**：`dec_up_128_128_160 W2=1` 命中 `onr_block_w2.glsl:96` `#error`；`dec_dfn_128_128 W2=1` 因 `NT <= 6` 丢通道使 Anchor #2 暴跌至 `10.2814 dB` → 永久关闭。

## 4. WP-D5 — `LT_W1A8` 勘误与无探针 `3×ABBA` 双分辨率终结

1. 修复 5 Bug 后 ACO 原生后端 `fp8=260/236/204`、`cvt_pk_fp8=496/416/384`，但实际寄存器达 **`216 / 216 / 240 VGPR`**（超 `<= 192` 目标，`scratch=0`）。其性能偏慢源于补全了完整通道投影、独立 `W_out` 计算及高寄存器与 `384–496` 条打包指令，而现行 `LT_VO` 在代数上融合了 `V·W_out`（权重于 `engine.cpp:250` 舍入为 FP16）。
2. Anchor #2 降 `−0.2910 dB`（vs `stage8-seal`）/ `−0.3270 dB`（vs `d1a`），无探针 `3×ABBA` **`4K +316.8 µs (+2.97%, 0/6)`**、**`1080p +73.9 µs (+2.65%, 0/6)`** → 彻底归档。

## 5. WP-D6 — Round 4 原型与 Round 5 `A2` 降级 + `D6a C=64×3` 多宽度晋级

1. **Round 4 → Round 5 `A2` 仲裁（`dec_dfn_128_128 LT_PV8=1` 退回 OFF）**：
   - Round 4 cumulative `@idx` 中 `dec_dfn_128_128_fl_lt −23.8 µs` 被 Round 5 单变量对照组（`r5-a2-idx-f{0,1}-4k` vs `r5-base-nopv8`）拆清：其中约 `−20.6 µs` 来自 ACO `unshackle`，`LT_PV8` 自身仅占 `_f_lt 180.3 -> 175.8 µs (−4.5 µs)` / `_fl_lt 128.2 -> 127.4 µs (−0.8 µs)`。
   - 400 帧无探针 `3×ABBA`：`r5-a2-abba400-f0-4k` `mean −5.02 µs (−0.048%, 5/6)`，但 **`r5-a2-abba400-f0-1080p` `mean +0.17 µs (+0.006%, 3/6)`**、**`r5-a2-abba400-f1-4k` `mean +6.63 µs (+0.063%, 2/6)`**。未达跨分辨率与流控晋级线，**在 `gpu/fastmode.cmake:20` 默认关回 OFF**（保留 `onr_block_fast.glsl` 实现可按需启用）。
2. **Round 5 `A1`（`D6a/D6b` `C>=64` 全宽度原型扩展与四组单变量实测）**：
   将 `LT_PV8` 扩至 `onr_block_w2.glsl`（`C=64, 96`）与 `onr_block_fast.glsl` `SPLIT == 2`（`C=160`）。所有 21 个变更核经 `RADV_DEBUG=shaders` 验证 **`global_load_u8 == 0`、`scratch == 0`**（`r5-a1-isa-summary.json`、`r5-a1-bench-summary.json`）：

| 试验组 | 覆盖核 | ISA 与资源 | Anchor #2 最差 (`vs nopv8` / `vs 4332470`) | `@idx 4K` 帧 / ksum (`µs`) | 无探针 `3×ABBA` (`4K` / `1080p`) | 终局裁决 |
|---|---|---|---|---|---|---|
| **`pv8-c64` (`D6a`)** | `enc_64`, `dec_up_64`, `dec_dfn_64` | 均稳守 **`240 VGPR / 9728 LDS`**；各 `+32 fp8 / −32 f16`（合计 `−96 f16`），`ds_write` `79->19 / 80->20 / 72->12` | `0` 非有限；**`−0.0427 dB`** (`Q_S1_4k_qual`) / **`−0.0899 dB`** (`Q_S2_1080p_qual`) | **`−59.0` / `−62.8`**（`enc_64 −12.1`, `dec_up_64 −17.5`, `dec_dfn_64 −21.1`） | 240f：`−48.62 (6/6)` / `−11.85 (6/6)`；**400f 复核：`−49.37 µs (−0.469%, 6/6)` / `−15.49 µs (−0.553%, 6/6)`** | **✅ 晋级锁定** (`fastmode.cmake:12,23,24`) |
| `pv8-c96` (`D6b`) | `enc_96`, `dec_up_96`, `dec_dfn_96` (`_f/_fl`) | `256 VGPR`（`dec_dfn_96 256->240`） | `−0.1431` / `−0.1921 dB`（超 `−0.10 dB` 线） | `−15.4` / `−7.2` | 240f：`−12.10 (6/6)` / **`+0.89 µs (+0.032%, 2/6)`** | ❌ 归档 |
| `pv8-c128-upenc` (`D6b`) | `enc_128`, `dec_up_128` (`_f/_fl`) | `256 / 240 VGPR` | `−0.0569` / `−0.1059 dB` | `−16.0` / `−15.7` | 240f：`−2.22 (4/6)` / **`+0.25 µs (+0.009%, 3/6)`** | ❌ 归档 |
| `pv8-c160` (`D6b`) | `enc_160×2`, `dec_up_160`, `dec_dfn_160` (`_f/_fl`) | **3 核由 `192 -> 256 VGPR`**（waves/SIMD `8->6`）；`dec_up_160` 保 `192` | `−0.0181` / `−0.0436 dB` | **`+97.6` / `+106.4`** | 240f：**`+112.72 µs (+1.070%, 0/6)`** / **`+51.06 µs (+1.823%, 0/6)`** | ❌ 归档 |

   *(注：其余 `Q·K^T` 与通道重排耦合的 `W_out` 如需扩展为 `act-scale`，无需重新训练权重，可采用 tile 在线求尺度或本地样本离线标定后再做独立场景验证，但须先实测确认在线缩放/打包不回吐收益或推高 VGPR 台阶。)*

## 6. Round 4～7 `[P1]` / `[P2]` 评测、数值安全与原子发布回滚 hardening

1. **`[P1]` `Fast=2` (`exp-aco`) 归因、`p8-unshackle-candidate` 实包对账与 `runtime` 元数据（`r6-p1-p8-reconciliation.json`）**：
   - 核查源码确认：**`asi/build/build_runtime_trace.sh:9` 原未传入 `-DONR_LT_DEEP="\"2,4,2,4\""`**（`set_deep` 关闭、特征显存 `209 MB`），因此 Stage 8（`s8_exp`）与 Round 3（`u9_exp`）测的均是无 level-4 时域复用的态；而正式 `build_asi.sh:39` 与 `gpu/CMakeLists.txt:242` 始终为 `openrr-amd.asi` 编译 `-DONR_LT_DEEP="\"2,4,2,4\""`（`f >= 4` 激活、`211 MB` VRAM）。Round 6 已同步为 `build_runtime_trace.sh:9` 补齐 `-DONR_LT_DEEP="\"2,4,2,4\""`，并在 `openrr_contrast_eval.cpp` `summary.json` 新增 `"runtime"` 块；Round 7（`d7b5e0a`）进一步将其收紧为 `gpu_executed`、配置推导值 `configured_deep` / `configured_deep_enabled` / `configured_deep_source` 与实测日志解析 `effective_flow`（`--no-gpu` 实测 `gpu_executed=false, effective_flow=null`，单用例 `P00` GPU 冒烟实测 `gpu_executed=true, effective_flow=0, P00=47.5665 dB, ACO=130/LLPC=0`）。
   - **修正 Round 5 基线误标并补齐 `p8` 对账**：Round 5 `r5-prev-u9-anchor2-exp-flow0` 加载日志实测命中 `pre-unshackle-9ebfb19` `94/94` 核（故 `P00 −0.0389 dB` / `Q_S1 f63 −0.0883 dB` 实为相对 `9ebfb19` 差值）。Round 6 使用同颗终版 `openrr-amd.asi`（`92cb14c8...`）、`FLOW=0`、默认 `ONR_LT_DEEP="2,4,2,4"` 隔离运行 `r6-final-anchor2-exp-flow0` 与 `r6-p8-anchor2-exp-flow0`（日志实测 `94/94` 匹配真 `p8-unshackle-candidate`、相对 `9ebfb19` 改 `33` 核、相对终包改 `15` 个 `code_hash` / `11` 个 SPV，零 LLPC）：全 `47` 张参考帧（`18` `v1` + `29` `v2`）`total_non_finite = 0`，相对真 `p8` 各用例最低差 `−0.0133`（`Q_S0_4k_perf`）/ `−0.0127`（`Q_S0_1080p_dlaa`）/ `+0.0223`（`Q_S1_1080p_qual`）/ `−0.0477`（`Q_S1_4k_qual`）/ **`−0.1017`**（`Q_S2_1080p_qual`）/ `+0.0190`（`Q_S2_4k_perf`）/ **`−0.0122 dB`**（`P00`），`Q_S1 f63 +0.0144 dB`，全 `47` 帧区间 `[−0.1017, +0.1141] dB`，`140/140` 帧直接比对 `non_finite = 0`；设 `ONR_RR_DEEP=0` 回到 **`48.5126 / 48.5272 dB`**；且 **`exp-aco` `FLOW=0` vs `FLOW=1` 在 `frame_compare.csv` `140/140` 帧 `diff_halfs=0, max_ulp=0`**。
2. **`[P2]` Anchor #2 评测器互锁硬化（`runtime/bench/openrr_contrast_eval.cpp`）**：全回读帧扫描 `NaN/Inf` 汇总 `total_non_finite`；缺参考显式记 `missing_ref/null`；`FLOW=0/1` 支持 `--compare-frames`；新增 `--dump-frames` 与 `--compare-frames` 同路径 `weakly_canonical` 前置拒绝（实测 `RC=1`）+ `--compare-frames` 先读后写顺序 + `summary.json` `"env"` 与 `"runtime"`（含 `gpu_executed` 与 `configured_deep*`）元数据块。
3. **`[P2]` `Item NFMA` 与 Direct D3D12 guides（`asi/tests/test_asi_units.cpp:1272-1960`）**：明确数值表达（`65504.0 = 0x7BFF` 可精确表示，超越最大正规阈值输入 `>= 65520`、测试用 `65600.0` 先溢成 `+Inf` 再由 `half_rz` 回退到 `0x7BFF`）；固定 `manifest.json`（`4 global + 37 native`），覆盖全部 4 支 `downsample{,_tl}_lt` / `d3d_downsample{,_tl}_lt`、`pooled_history2_lt`、`k_lite/k_lx` 全 22 核及 `Item NFMA Direct D3D12 guides`（`qNaN/±Inf/-0/subnorm/±65504` 下 `New=1`/`Fast=2` `0` 非有限）；Round 6 对刷新后的 `manifest.msgpack` 复跑 `test_asi_units.exe` 全绿 PASS。
4. **`[P2]` 原子发布/回滚闭环、`msgpack` 索引校验与 `gfx11` 作用域（`build_aco_records.py`、`build_shaders.py`、`gpu/CMakeLists.txt`）**：捕获/合包全量写入 `.openrr-d3d12-v1.staging.<pid>` 校验；延迟正式目录生成并在捕获失败时保留已一致旧包的 `manifest.json`/`manifest.msgpack`；借确定性 `.openrr-d3d12-v1.backup` 完成 `os.replace` 切换、异常回滚，且仅在新包就位校验后删除备份（双故障保留备份并在下次启动自动恢复，局部 `.nrp` 损坏仍复用未损缓存）；Round 7 在 `_is_consistent_package()` 中增加 `_unpack_msgpack` 与 `_expected_index` 强校验 `manifest.msgpack`（核对 `manifest_hash` 与全部 `records` 元数据及头部字段），阻止启动误清 `.backup`；`test_aco_build_policy.py` **`17/17 OK`** 覆盖上述故障注入（含错配/篡改 `manifest.msgpack` 恢复回归）及 `--gfx11` 剥离 `W2_96UP=1` 验证；另明文标注较 `pre-unshackle-9ebfb19` 变更的 `6` 支 `shaders-gfx11` `attr/post` 着色器尚缺 RDNA3 实机覆盖，当前发行签收范围限定 RX 9070 XT / `gfx1201`。
5. **洁净复验与终版封板提交**：Round 5 `OPENRR_CLEAN_STAGE=1` 全新重构于 `f6dcc83`；Round 7 更新打包/评测工具后基于缓存刷新 `manifest.json`（`38242555...`）/`manifest.msgpack`（`c8af6621...`，第 2 次运行 `0/94` 重捕获），`openrr-amd.asi`（`92cb14c8...`）及 `94/94` `.spv` / `.nrp` 与 `f6dcc83` 逐位一致，交付包 `build/aco-v015/stage/` 与 `final-pkg/stage/` 全程维持 **`296/296` SHA-256 一致、`0` `.log` 残留**；代码提交于 `codex/mesa-263-comparison` **`d7b5e0a`**，全套数据详见 [`v016-stage9-wave-d-final-audit.md`](v016-stage9-wave-d-final-audit.md)。
