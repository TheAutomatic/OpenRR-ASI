# Stage 8 Phase 2–6：FP8 攻坚、平面编码修复、属性核优化、Fast=2 交付与多语言系统（2026-11-16 终局复核）

> 代码封板提交：`e745d71`（`codex/mesa-263-comparison`，紧邻父基线为 Phase 1 合流基线 `frozen-016-base` / `897c18e`）。全部实验原始件位于 `build/v015-runs/stage8/<tag>/`。A/A 噪声带：`aa-4k` 均值 `−0.0264%`（中位 `+0.0137%`，对带 ±0.23%）、`aa-1080` 均值 `−0.0359%`（中位 `−0.0900%`，对带 ±0.26%）；未改动核 ±0.5% 为异常排查触发线。

---

## 1. WP-C1（已晋级，`5721569`）：W2 `C=64` 与 TR `C=160` 出口一次量化 `FP8QV`

- **设计**：TR `C=160`（`dec_up_160_256_160`）启用 `XQ8Y` 路径；W2 `C=64`（`enc_stage_64_64_96`、`dec_up_64_64_96`）补齐 `qe4_pair32` 出口一次量化供逐 head `Q/V` FP8 WMMA 复用（plane 直读分支保留原路径）。
- **静态与资源**：`manifest.json` 恰 4 条 `.nrp` 变化（`enc_stage_64_64_96_f_lt`、`dec_up_64_64_96_f_lt`、`dec_up_160_256_160_f_lt`、`dec_up_160_256_160_fl_lt`），零 VGPR 台阶、零 LDS 增量、`scratch=0`。
- **实测门禁**（`wpc1-idx-4k` / `wpc1-abba-*` / `wpc1-anchor2`）：
  - `@idx 4K`：paired total `10862.3 -> 10814.3 µs`（`−48.0 µs / −0.44%`），其中 `dec_up_160_256_160_fl_lt` `−5.5 µs（−5.51%）`、`dec_up_64_64_96_f_lt` `−18.1 µs（−2.32%）`、`enc_stage_64_64_96_f_lt` `−17.9 µs（−1.89%）`。
  - **3×ABBA**：4K 均值 **`−0.3186%`**（中位 `−0.3098%`，`6/6` 负，`10.94682 vs 10.98182 ms`）；1080p 均值 **`−0.3803%`**（中位 `−0.4103%`，`6/6` 负，`2.91083 vs 2.92195 ms`）。
  - Anchor #2：`v1` `76/76` + `v2` `44/44` 全绿，`non_finite=0`，vs `4332470` `v2` 最差帧 `Q_S1_1080p_qual f67` `39.1239 dB`（`−0.1938 dB`）。

---

## 2. Phase 4（已晋级，`4670e28`）：Fast=2（`_lx`）语义对齐与时间域验收

- **设计**：`gpu/shaders/include/onr_block_w1.glsl` 补全 `W1_GELU8` 在 `LT_GELU`/`LT_GELU_PART` 下的 2-op 线性门直喂 `qe4_pair32` 分派；`gpu/fastmode.cmake` 对 `LX_DEFS_*` 同步 `LT_MLP8=1`（`enc0` 另加 `LT_EMB8=1`）。
- **静态**：仅 4 个 `_lx` 核变化；`dec_up_32_64_64_f_lx` 静态 **VGPR 216 → 192**，`exec_bytes 18704 → 15608 B`（最终包）。
- **实测**（`p4-abba-*`、`p4-steady-*`、`final-remed-anchor2-exp`）：
  - **Fast→Fast 3×ABBA（`p4-abba` vs Phase 1 `_lx`）**：4K **`−1.7982%`** 均值 / `−1.7649%` 中位（`6/6` 负，`10.58987 vs 10.78378 ms`）；1080p **`−1.7581%`** 均值 / `−1.7265%` 中位（`6/6` 负，`2.82805 vs 2.87866 ms`）。
  - 最终封板 `Fast=2` 稳态（`final-remed-steady-*-f2`，`500w/1500w+600f`）：4K 中位 **`10.51570 ms`**（基线 `10.97760 ms`，`3.61x` 5090）、1080p 中位 **`2.78768 ms`**（基线 `2.91888 ms`，`3.20x` 5090），`non_finite=0`。
  - 最终 `Fast=2` 时间域画质（`final-remed-anchor2-exp`，`exp-aco`）：`v1` **`76/76`**（全用例最低 `Q_S0_4k_perf = 51.4244 dB`，`Q_S0_1080p_dlaa = 51.7197 dB`）、`v2_motion` **`44/44`**（全用例最低 `Q_S1_1080p_qual = 38.9676 dB`，`P00 = 48.5068 dB`，`Q_S2_1080p_qual = 40.3296 dB`），`non_finite=0`。

---

## 3. WP-C4（已修复并晋级，`de7dedc`）：全快链 OCP `e4m3fn` 平面编解码 `LT_E8RAW=1`

- **前置工程修补**：
  1. 修复 `gpu/shaders/include/onr_block_fast.glsl` 中 `REC2Y` 宏条件编译误落入 `#ifdef LT_E8RAW` 的 `#else` 分支问题，将其移至独立 `#ifndef REC2Y`。
  2. 在 `asi/tests/test_asi_units.cpp` 建立 `Item OCP`：穷举 256 个 `e4m3` 字节码的 OCP 精确解码与 `sm_86` 解码边界（正规区逐位一致，指数 `0` 的 16 个码 `sm_86` 误读为 `(1+m/8)·2^-7` 而非 `±0`/次正规数 `m·2^-9`，`0x7f`/`0xff` OCP 为 NaN 而 `sm_86` 读 `±480`），并钉死 4-lane `(0,1)/(2,3)` 字序往返。
  3. 对齐全部 3 族生产者/消费者（包含 `get16` 与 `XOUTQ` 默认路径的 `e4m3x4`/`e4m3_out2`、`ocp_e4m3_in/out` 及矩阵/池化消费端）与 `build_shaders.py` `--gfx11` 剥离链（保持 `shaders-gfx11` 94 核零 FP8 能力与零 `Float8` 字符串）。
- **静态与资源**：27 个 `_lt`/`_lx` 核 `exec_bytes` 显著下降（如 `enc_stage_128_128_160_fl_lt` `-8,460 B`、`enc_stage_96_128_128_f_lt` `-4,424 B`、`enc_stage_160_256_160_fl_lt` `-3,648 B`、`dec_head_32_64_f_lx` `-2,860 B`），VGPR / LDS / `scratch=0` 全无恶化。
- **实测门禁**：
  - `@idx 4K`（`e8raw-remed-idx-4k`）：paired total `10809.8 -> 10644.1 µs`（**`−165.7 µs / −1.53%`**；`dec_up_32 −28.7 µs`、`enc_64 −24.0 µs`、`dec_up_64 −15.8 µs`、`dec_head_32 −14.3 µs`）。
  - **3×ABBA**：4K 均值 **`−1.5245%`**（中位 `−1.5161%`，`6/6` 负 `[−1.5571, −1.6747, −1.4674, −1.6175, −1.4751, −1.3552]%`，`10.65407 vs 10.81898 ms`）；1080p 均值 **`−1.9278%`**（中位 `−1.9041%`，`6/6` 负 `[−1.9538, −2.0395, −1.7961, −1.8544, −2.1775, −1.7453]%`，`2.84991 vs 2.90593 ms`）。
  - Anchor #2（`e8raw-remed-anchor2` vs `remed-base-anchor2`）：`v1` `76/76`（全用例最低 `52.0338 -> 52.0809 dB`，**`+0.0471 dB`**），`v2` `44/44`（全用例最低 `39.1239 -> 39.1391 dB`，**`+0.0152 dB`**，单用例最差 `Q_S2_1080p_qual −0.0182 dB`），`non_finite=0`。
  - `test_asi_units.exe` 全绿（含 `Item R10`、`Item QV`、`Item OCP`）。

---

## 4. WP-C5（对照③深化与 D2 晋级，`09246e0`）：每核捕获覆盖、`enc0_attr` `LT_ATTR_RCP=1` 晋级与编译器假设证伪

- **工程基础设施（`09246e0`）**：
  - `asi/build/mesa/prepare_capture.py` + `build_capture_263.sh`（`patch_version: 4`）新增每着色器捕获控制：`ONR_ACO_CU_MODE`、`ONR_ACO_WGP_MODE`、`ONR_ACO_SAMPLE_WAIT_ZERO`；`build_aco_records.py` 新增 `--cu-mode-shaders`、`--wgp-mode-shaders`、`--sample-wait-zero-shaders`；`asi/tests/test_aco_build_policy.py` 扩展至 **7/7 单测通过**。
- **试验 D0（纯编译器开关，已证伪移除）**（`c5-remed-idx-4k`）：
  - `--wgp-mode-shaders` 使 `enc_stage_160_256_0_fl_lt` `+12.08%`、`dec_dfn_160_256_fl_lt` `+5.73%`（paired total `+34.1 µs / +0.32%`）；`--cu-mode-shaders` 对 `d3d_enc0_attr_f_lt` 为 `+0.1 µs`；`--sample-wait-zero-shaders` 对 `post_f_lt` 为 `+0.2 µs`。全部 3 个覆盖未合入 `build_asi.sh`。
- **试验 D1（`post_f_lt` `[[dont_unroll]]`，已证伪回退）**（`c5b-remed-idx-4k`）：
  - `post_f_lt` 由 `1225.7 -> 1265.2 µs`（**`+39.5 µs / +3.22%`**），已干净回退（`onr_post_main.glsl` 恢复原展开调度）。
- **试验 D2（`enc0_attr` `LT_ATTR_RCP=1`，已晋级，`09246e0`）**：
  - 在 `gpu/fastmode.cmake` `LT_DEFS_enc0_attr` 启用 `LT_ATTR_RCP=1`（`total_records=94`、`legacy_diffs=[]`、`gfx11` 零 FP8、`lt_diffs` 恰含 4 个 `enc0_attr*_lt` 变体）。
  - 静态：`d3d_enc0_attr_f_lt` 维持 **96 VGPR / 0 LDS / 0 scratch**，`exec_bytes 26600 -> 22524 B`（`−4,076 B`）。
  - `@idx 4K`（`d2-remed-idx-4k`）：`d3d_enc0_attr_f_lt` `930.2 -> 854.8 µs`（**`−75.4 µs / −8.11%`**），paired total `10610.3 -> 10541.0 µs`（`−69.3 µs / −0.65%`）。
  - **3×ABBA**：4K 均值 **`−0.6209%`**（中位 `−0.6524%`，`6/6` 负 `[−0.7814, −0.3165, −0.6213, −0.5231, −0.6836, −0.7996]%`，`10.60358 vs 10.66985 ms`）；1080p 均值 **`−0.7126%`**（中位 `−0.7219%`，`6/6` 负 `[−0.8905, −0.3933, −0.6972, −0.6720, −0.8762, −0.7466]%`，`2.82473 vs 2.84501 ms`）。
  - Anchor #2（`d2-remed-anchor2`）：`v1` `76/76`（`+0.0000 dB`）、`v2` `44/44`（`39.1391 -> 39.1378 dB`，最差 `P00 −0.0622 dB`）、`non_finite=0`。
  - 同包对照③（`d2-llpc-idx-4k`）：`d3d_enc0_attr_f_lt` 对 LLPC 逆差从合流期 `~75 µs` 压缩至 `-19.1 µs（-2.23%）`；整帧 ACO 领先 LLPC **`+2255.7 µs（+21.36%）`**。

---

## 5. Phase 5（已交付，`e745d71`）：四语热切换菜单系统、Vulkan 队列锁加固与冷装验收

- **完整实现**：
  - [`asi/src/rr_lang.hpp`](../../asi/src/rr_lang.hpp)：`msgid/msgstr` 解析（UTF-8 BOM、CRLF、转义与续行、`@language`/`@font`）、`conversions()` `printf` 格式串安全门（`rejected` 计数防格式崩溃）、BMP 非 ASCII 码点收集、`Label##id` 与 `tr_items()`、1 秒节流热重载、`suspended()` 缺字形保护。
  - [`asi/build/make_lang.py`](../../asi/build/make_lang.py)：词法提取 `tr(...)` 与 `tr_items(...)`（自动按 `\0` 拆分下拉项），产出 `English.txt` **71 个非 `@` 词条**。
  - [`asi/src/rr_pe_menu.cpp`](../../asi/src/rr_pe_menu.cpp)：`Menu::draw()` 全部 71 个用户可见控件、状态行、下拉选项与提示均完成 `tr()`/`tr_items()` 包裹（含 `[OpenRR] Language` 下拉与 `tr_id` 保持折叠面板在热切换时不意外闭合）；`fonts_for()` 以 UTF-8 路径构建合并字体回退链（`@font` → `lang\fonts\NotoSansCJKsc-Regular.ttf` → `C:\Windows\Fonts\msyh.ttc` / `simhei.ttf`），修正 `FontDataOwnedByAtlas=true` 防重复加载 21.6 MB 字体泄漏，并在 `io.Fonts->Build()` 后通过 `ImFontBaked::FindGlyphNoFallback` 全量校对所有码点。
  - [`asi/src/rr_pe_menu_vk.inl`](../../asi/src/rr_pe_menu_vk.inl)：在 `VulkanOverlay::record()` 进入 RenderPass 前遍历 `draw_data->Textures`，为需要创建的纹理更新加 `openrr::pe::vkdevice::lock_queue_held(queue)` / RAII 解锁保护后再调用 `ImGui_ImplVulkan_UpdateTexture`，防止 ImGui 字体图集动态重建时与渲染队列并发抢占 `VkQueue`。
  - [`asi/lang/`](../../asi/lang/)：`English.txt`、`zh-CN.txt`、`ja.txt`、`ko.txt` 达成 **71/71 词条 100% 键对齐**、`0` 缺失/`0` 多余/`0` 空值/`0` 未译/`0` 格式串不匹配/`0` `NotoSansCJKsc-Regular.ttf` 缺字；[`asi/build/build_asi.sh`](../../asi/build/build_asi.sh) 随包生成并部署 `lang/*.txt` 与 `lang/fonts/{README.txt,Noto-CJK-LICENSE.txt,NotoSansCJKsc-Regular.ttf}`（`21,659,488 B`，`.gitignore` 排除 `asi/lang/fonts/*.ttf`，`THIRD_PARTY.md` 补充 SIL OFL 1.1 声明）。
- **无头验收与冷装验收**：
  - [`Item LANG`](../../asi/tests/test_asi_units.cpp) 通过：`list()` 发现 4 语、`zh-CN/ja/ko` 均 `entries=71 >= 55` 且 `rejected=0`、`NotoSansCJKsc-Regular.ttf` 随 stage 存在、`Ini` + Win32 Profile INI 往返全绿（`test_asi_units.exe` 31 条 `[PASS]`，`EXIT=0`）。
  - **全新空白目录冷装验收**：将 `build/aco-v015/stage/*` 完整拷入全新空白目录 `%TEMP%\openrr-stage8-cold-install` → `94` shaders / `94` shaders-gfx11 / `94` `.nrp` / 4 语 `.txt` / `NotoSansCJKsc-Regular.ttf` 完整 → `openrr_bench.exe --runtime <cold>\plugins\openrr-amd.asi --data-dir <cold>\openrr-amd` @1080p (`48w+64f`) 成功创建、`94/94` ACO 记录命中、中位 **`2.81332 ms`**、`parity_passed=true`、`COLD_EXIT=0`（用后已清理临时目录）。
- **晨审 2 分钟实机交互热切换清单**（D3D11/D3D12/Vulkan 三后端 UI 目检需游戏内会话）：
  1. 将 `build/aco-v015/stage/` 内部文件拷入测试游戏根目录；
  2. 打开菜单 → Menu & Hotkeys → `Language`：依次热切换 `English` → `简体中文` → `日本語` → `한국어` → `English`（观察折叠区保持展开、单帧内无闪烁/无崩溃/无豆腐块）；
  3. 修改 `openrr-amd.ini` `[OpenRR] Language = zh-CN` 冷启动，并直接编辑 `openrr-amd\lang\zh-CN.txt` 验证 1 秒内热重载。

---

## 6. 已证伪 / 搁置项与五项证据 / 重开条件清单

| 条目 | 实测处置 | 五项关键证据与统计 | 明确重开条件 |
|---|---|---|---|
| **C=160 WGP 模式**（Phase 1） | 判负保留 CU | `wgp-idx-4k`: `enc_160_160_fl +17.7 µs (+15.68%)`, `dec_up_160_fl +13.2 µs (+13.13%)`, `dec_dfn_160_fl -10.1 µs`, paired total **`+54.8 µs (+0.51%)`**; `wgp-abba-4k`: **`+0.2685%`** mean (`+0.2676%` med, `0/6` 负) 出噪声带; `scratch=0` | `C=160` 家族 LDS/tile 重切使工作组扩倍收益反超同步损失 |
| **WP-C2 Step 2A**（W1 Q FP8） | 判负封存（仅代码惰性/未开启） | 静态 VGPR **192 → 216**（踩中 16→12 wave 台阶）; `w2a-idx-4k`: `enc0 +13.0 µs`, `dec_up_32 +12.9 µs`, `dec_head_32 +9.7 µs`, paired `+11.1 µs`; `w2a-abba-4k`: **`+0.1967%`** mean (`+0.2477%` med, `0/6` 负), `1080p +0.0083%` (`2/6`); Anchor #2 `v2` 最差 `Q_S2_4k_perf f63` **`−1.0713 dB`**（LR 舍入误差在运动场景放大） | 按 token-loop 重构压回 `≤192 VGPR` 且键矩阵采用每通道/每 tile 缩放或混合精度，避免 LR 直舍入崩塌 |
| **WP-C2 Step 2B/2C**（`f769d25`, `LT_VO8` 192-VGPR 0-LDS） | 已完成上机评测并关闭门控 | 源码：`upload_qv`/`UQ` `[5n, 6n)` `e4m3_rne(lr)` 尾段 + `onr_w1_head.glsl` `[[dont_unroll]] vt_=0..3` 循环；静态全部 8 个 `32_64` `_lt`/`_lx` 核维持 **192 VGPR / 0..4096 B LDS / 0 scratch**；`vo8-remed-idx-4k` 3 个 W1 核全面退化：`enc0 +63.6 µs (+4.83%)`、`dec_up_32 +65.7 µs (+5.93%)`、`dec_head_32 +80.8 µs (+6.90%)`，paired **`+232.8 µs (+2.22%)`**；`vo8-remed-anchor2` 失败：`v1 −2.4160 dB`、`v2 −0.9407 dB`、`P00 −3.0376 dB`；关闭后 `lt_diffs=[]` | 解决 `[[dont_unroll]]` 指令序列化惩罚 + 改用每组缩放/混合精度保 VO 数值动态范围 |
| **WP-C3**（`DFN_PRE` `x2^T` 与 `W_out/S/O`） | 桌面周期账搁置 | 阅读 `onr_block_fast.glsl` 证实 `DFN_PRE_T` 消费 `x2^T`（line ~1194，MLP+残差 2 后）而非 `LT_MLP8` 的 `x1^T`；每 16-K 仅 1 次 WMMA（`N=16`），单独量化 `x2^T` 无法回本；`W_out`/`S`/`O` 均已 tile 化且非 WMMA 带宽瓶颈 | 未来与 `C≥96` `XOUT`/`XOUTQ` 共享同源 `x2^T` 量化，或 RGP 实测证实 `W_out` WMMA 停顿占比 >5% |
| **WP-C6**（`dec_up_128` 强制 `SPLIT=2`） | 判负回退 | 静态 VGPR `240→192`、exec `−7.7 KB`；`c6-idx-4k`: `dec_up_128 +2.4 µs`（在先前同包比较中 `−5.1 µs`），下游未改动 `dec_dfn_128_fl` 耦合至 `155.2 -> 187.7 µs (+32.5 µs / +20.94%)`；`c6-abba-4k`: **`+0.0784%`** mean / `+0.0250%` med（`3/6` 负，噪声内） | `dec_up_128` 与 `dec_dfn_128` 成对联合重切 LDS/wave 映射，避免单边改写破坏下游局部性 |
| **WP-C7**（核间气泡 stamps 普查 vs RGP） | 已完成无头邮戳普查，RGP 留交互时段 | 按 [`idx_report.py`](../../build/v015-runs/stage8/idx_report.py) 对齐 `@2`/`@42` 核与核后间隙（`final-remed-idx-4k` 596 帧均值）：22 次派发核总和 `10582.0 µs`，**核后总间隙 `229.4 µs（约 2.1%）`**，头号气泡 **`32:dec_dfn_96_128_fl_lt -> 34:dec_up_64 = 116.5 µs`**（基线 `120.5 µs`）、次席 **`42:post_f_lt -> 44:d3d_downsample_lt = 102.5 µs`**，其余 20 处均 `0.0–2.9 µs` | 目标机开启交互式 AMD RGP 会话抓取 wave 占用/barrier 排空事件级剖面 |
