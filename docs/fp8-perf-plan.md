# OpenRR (`openrr-amd.asi`) 缺陷修复 + RDNA4 性能 / 显存优化计划

> 工作分支：一律在 `pr/openrr-amd-asi` 上改与提交；`main` 只跟上游同步，本文不出现合并/合入分支的说法。

原则（全程有效）：

- **只保留实测正向提速项**；任一目标分辨率下变慢的开关不得进默认产物。
- Legacy（`--fast-mode 0`）保持逐位一致（`max_ulp_diff == 0`），除非该阶段明确标为有损并单独门禁。
- 不弯曲 mochi / NR 壳去迁就 OpenRR；系统级方案优先于打补丁式特判。
- 每阶段结束跑 `test_asi_units.exe` + `openrr_bench.exe`（1080p / 1440p / 4K），写进 `docs/test-evidence/`。

---

## 基线提交（`B0 + B1 + B2`）— ✅ 已完成并提交归档

> **当前状态：已完成（`pr/openrr-amd-asi` @ `90fea26` + `asi/docs` 基线提交）**  
> `B0`（移除默认 W1 `LDS_OCC_PAD=8192` + SPIR-V 哈希门禁）、`B1`（`t0` 尾部无损池化 + `wide` 原地替换 `<= +16 B/px` + 29 子缓冲零重叠审计 + `0dc2625` 顺序累加 `f0+0` 反向对照）、`B2`（3 轮逐宏 A/B 筛选 `LT_POST_EXP/NOCORNER/NETH`）均已验证通过并提交，历史证据与原始跑分日志已归档至 [`docs/test-evidence/`](test-evidence/)。下一阶段为 **阶段 2（输入端零额外量化损失真 FP8 `floate4m3_t` WMMA）**。

把**还没修完的 bug**、**确认无损的显存池收尾（B1）**与**实测正向的 Fast 模式近似快路径（B2，有损）**收成一次基线，在 `pr/openrr-amd-asi` 上提交。  
基线通过前，不开始后面的 FP8 / `hist_` 压缩项。

### B0. 撤回默认产物里的 W1 `LDS_OCC_PAD`（当前唯一未关缺陷）

见 [`pr-review-d6f70fb.md`](../pr-review-d6f70fb.md)。1080p W1 合计 −5.0%，**4K 合计 +5.8%**，却写进了默认 `fastmode.cmake` / `build_shaders.py`。

做法（基线采用 **默认全关**；1080p 实验室 A/B 可留钩子，不进 stage 包）：

1. [`gpu/fastmode.cmake`](../gpu/fastmode.cmake) 三个 W1 `LT_DEFS_*` 去掉 `LDS_OCC_PAD=8192`。
2. [`asi/build/build_shaders.py`](../asi/build/build_shaders.py) 删除 `w1_occ_kernels` 及对 `_f` / `_f_lt` / `_f_w` 的注入。
3. 保留 glsl 里 `#ifdef LDS_OCC_PAD` 与旁路 `occ_*` 目录，供以后实测，不进默认产物。
4. K2 确定性门禁：校验发布出去的 Top-5 SPIR-V 与 `occ_nopad` 版本 FNV-1a 64-bit 哈希完全一致（且与 `occ_pad16k` 不同），防止未来误把 `LDS_OCC_PAD` 开回默认。
5. 重跑 1080p / 4K 证据与 README 矩阵标签（去掉与事实不符的默认 `W1_occ16K`）。

### B1. 无损显存池收尾（Fast 模式 `t0` 尾部池化，严格无损）

[`gpu/src/engine.cpp`](../gpu/src/engine.cpp) 池化段：Fast 模式下，`enc2 .. dec2_1` 在 flow 链中无 barrier 并发执行，`f0` 头部的 `enc_l_[1].pooled` 与 `enc_l_[1].dfn_pool` 在 flow 期间仍被 `enc2` 读取，不可与 `dec1_l_[2..4].tensor` 复用；而 `t0`（`attr_`，大小 `px0 * 48`）在所有 `enc_l_[1..5]` 子缓冲与 `dec1_l_[1].tensor` 之后仍有空闲尾部（`px0 * 42.4 .. px0 * 46.6 < px0 * 48`，且远高于 `dec_head_32_64` 写出的 `px0 * 40` `headScratch`）。因此将 `dec1_l_[2..4].tensor` 与 `dec1_l_[1..4].dfn` 并入 `t0` 尾部。同时修正 `dec0ScratchBytes` 计算以包含 `enc_l_[0].dfn_pool` 的真实终点（并断言 `dOff <= dec0_l_[0].tensor.size && aOff <= attr_.size`），在 `has_flow_buffer_overlap()` 中覆盖全部 29 个 flow 并发子缓冲（含 `enc_l_[0].dfn_pool` 与 `dec0_l_[1..4].tensor`），并修复 `wide` 路径（`attr_wide_`）通过 `create_unzeroed` 原地替换 + 墓地延迟释放（graveyard）与 `dec1_l_[1..4]` 地址同步平移，既不在录制中途发 `clear_range` 或立即释放旧缓冲，又将 `wide` 显存增量保持在 `<= +16 B/px`（1080p `+5.68 MB`，4K `+22.21 MB`）。

门禁：同一二进制单独开关 B1（`set_pool_dec1_tail(false/true)`，包含第 10..14 帧打开 `depth_of_field` 触发 `wide` 路径，以及 `set_pool_dec1_f0_race_for_test(true)` 按 `0dc2625` 顺序累加 `d1Off` 还原 `f0+0` 竞争布局的反向对照），1080p 与 4K 连跑 20 帧 × 2 轮，`diff_vs_unpooled == 0`、`repeat_diff == 0`、`flow_overlap_audit == CLEAN`（反向对照 `neg_ctrl_f0_race_caught == YES`，1080p `race_diff = 38,875,537`、4K `race_diff = 153,478,994`）；节省显存：1080p `1.44 MB`（feature `252 → 251 MB`，`proc_delta` `−2.3 MB`），1440p `2.56 MB`（feature `438 → 435 MB`，`proc_delta` `−2.8 MB`），4K `5.62 MB`（feature `966 → 960 MB`，`proc_delta` `−6.7 MB`）。

### B2. 逐项打开实测正向的 `LT_*`（Fast 模式有损近似快路径）

> **注意**：B2 中的 `LT_*` 宏是 Fast 模式（`_lt`）的**有损数学近似**（相对 `d6f70fb` Fast：1080p `PSNR_01 = 55.53 dB`、`PSNR_tonemap = 59.93 dB`、`p99_abs_err = 0.005371`，4K `PSNR_01 = 60.42 dB`、`PSNR_tonemap = 65.20 dB`、`p99_abs_err = 0.003174`，`non_finite = 0`；`openrr_bench` 门禁已收紧为 `PSNR_01 >= 54.0 dB && PSNR_tonemap >= 58.0 dB && p99_abs_err <= 0.006`；Legacy `--fast-mode 0` 不受影响，保持 `max_ulp_diff == 0`）。

在 [`gpu/fastmode.cmake`](../gpu/fastmode.cmake) **逐宏交替跑 3 轮取中位数 `[min..max]`** A/B，严格执行「1080p 与 4K 都明确变快才写进默认」规则：

| 宏组 | 1080p 3 轮中位 `[min..max]` (`test_asi_units`) | 4K 3 轮中位 `[min..max]` (`test_asi_units`) | 决策 |
|---|---|---|---|
| `post`：`LT_POST_EXP=1 LT_POST_NOCORNER=1 LT_POST_NETH=1` | `346.3 [344.1..353.4] → 324.2 [311.2..326.8] us` (**−6.4%**, −22.1 us) | `1497.1 [1456.2..1501.6] → 1405.6 [1395.6..1408.4] us` (**−6.1%**, −91.5 us) | **默认开启** |
| `post`：`+LT_POST_LOG=1`（对比上一行） | `324.2 → 349.4 [329.9..349.5] us` (+7.8%) | `1405.6 → 1495.0 [1485.2..1497.3] us` (+6.4%) | **关闭**（RDNA4 硬件 `v_log_f32`/`v_exp_f32` 更快） |
| `enc0_attr`：`LT_ATTR_RCP=1 LT_ATTR_DEPTH=1` | `176.4 [175.5..180.5] → 180.6 [178.2..181.0] us` (+2.4%) | `816.6 [816.6..822.5] → 814.6 [813.5..815.4] us` (−0.2%, 噪声内) | **关闭**（两档均在噪声内，不值得付有损代价） |
| `enc0_attr`：`+LT_ATTR_SAME=1 LT_ATTR_CL=1`（对比上一行） | `180.6 → 243.4 [243.0..243.7] us` (+34.8%) | `814.6 → 985.8 [985.4..989.9] us` (+21.0%, +171.2 us) | **关闭**（额外分支负向） |
| `C=64`（×3 合计）：`LT_FAR=3` | `712.3 [710.8..713.9] → 709.1 [709.0..716.4] us` (−0.5%) | `2664.6 [2662.8..2668.4] → 2697.5 [2689.6..2703.1] us` (+1.2%) | **关闭** |

- **算子收益与端到端差异说明**：
  - 仅开启 `LT_POST_EXP + LT_POST_NOCORNER + LT_POST_NETH`（并配合 `B0` 移除 W1 `LDS_OCC_PAD=8192`）后：
    - **1080p 端到端收益大于算子之和，原因未定位**：1080p 下 `post_f_lt` 算子省 `~20–22 us`（`346.3 → 324.2 us`），`B0` 移除 W1 `LDS_OCC_PAD` 在 1080p 下处于噪声范围（`W1` `nopad=1024.6 us` vs `occ_pad_16KB=1022.5 us`），但同进程端到端 3 轮中位数改善为 **`−47 us`（`3.171 → 3.124 ms`，`−1.5%`）**，端到端比算子单项之和多出约 `25–30 us`，具体原因未定位。
    - **4K 算子与端到端大致吻合**：`post_f_lt` 算子省 `~91 us`（`1497.1 → 1405.6 us`），`B0` 移除 W1 `LDS_OCC_PAD` 省 `~60–76 us`（`4189.5 → 4113.6 us`），算子合计省 `~151–167 us`，同进程端到端 3 轮中位数改善 **`−129 us`（`11.941 → 11.812 ms`，`−1.1%`）**，两者大致吻合。
  - 后续阶段 2 / 3 的收益评估一律以同进程端到端 A/B 为准，不按算子耗时线性相加。

### 基线完成定义（✅ 全部达成）

- [x] B0 默认产物无 W1 `LDS_OCC_PAD`；K2 SPIR-V 哈希确定性门禁落地（`spv_hash_nopad=EXACT`）；证据 + README 与产物一致
- [x] B1 `t0` 尾部无竞争池化落地，单独开关 B1 + `wide` 路径（`<= +16 B/px`）+ `f0+0` 反向对照跑 20 帧 × 2 轮 `max_ulp_diff == 0`、`repeat_diff == 0`、`flow_overlap_audit == CLEAN`、`neg_ctrl_f0_race_caught == YES`（节省 `1.44 / 2.56 / 5.62 MB`，feature `252→251 / 438→435 / 966→960 MB`）
- [x] B2 标明「Fast 模式有损近似」（门禁收紧为 `PSNR_01 >= 54.0 dB && PSNR_tonemap >= 58.0 dB && p99_abs_err <= 0.006 && non_finite == 0`），仅启用 1080p 与 4K 3 轮中位数均明确变快的 `LT_POST_EXP/NOCORNER/NETH`
- [x] `test_asi_units` 全过；`openrr_bench` 以 `d6f70fb` ASI 为 `--baseline-runtime` 在同一进程跑 3 轮 1080p/1440p/4K A/B 对照并归档至 [`docs/test-evidence/runs/2026-11-08-b0-b1-b2-baseline/`](test-evidence/runs/2026-11-08-b0-b1-b2-baseline/) 与 [`docs/test-evidence/history/`](test-evidence/history/)
- [x] **已在 `pr/openrr-amd-asi` 提交（`90fea26` + `asi/docs` 基线提交）**

---

## 现状快照（RX 9070 XT，同进程 3 轮 A/B 对比 `d6f70fb` 中位数，2026-11-08）

| 分辨率 | `d6f70fb` Fast `median` (3 轮范围) | **基线 (`B0+B1+B2`) Fast `median` (3 轮范围)** | 同次 A/B 变化 | 显存（`saved` / feature / `proc_delta`） | Fast Parity (vs `d6f70fb`) |
|---|---:|---:|---:|---:|---|
| 1080p Quality | `3.171 ms` (`3.167–3.178`) | **`3.124 ms`** (`3.121–3.127`) | **−1.5% (−47 us)** | **`−1.44 MB`** (`251.0 MB` / `−2.3 MB`) | `PSNR_01=55.53 dB`, `PSNR_tm=59.93 dB`, `p99=0.0054` |
| 1440p Quality | `5.451 ms` (`5.439–5.459`) | **`5.391 ms`** (`5.363–5.403`) | **−1.1% (−60 us)** | **`−2.56 MB`** (`435.0 MB` / `−2.8 MB`) | `PSNR_01=56.18 dB`, `PSNR_tm=60.57 dB`, `p99=0.0049` |
| 4K Quality | `11.941 ms` (`11.934–11.966`) | **`11.812 ms`** (`11.749–11.824`) | **−1.1% (−129 us)** | **`−5.62 MB`** (`960.0 MB` / `−6.7 MB`) | `PSNR_01=60.42 dB`, `PSNR_tm=65.20 dB`, `p99=0.0032` |

---

## 基线之后（当前推进中：阶段 2）

### 阶段 2：输入端「零额外量化损失」真 FP8（`floate4m3_t`）— ⏳ 待开始

- **硬件范围说明**：本分支只支持 **RDNA4（`gfx12`）**，`Engine::set_fp8_ok(bool)` 为预留接口（当前运行时、ASI 与已启用的 `onr_block_fast.glsl` / `onr_block_w2.glsl` 均要求 RDNA4 `VK_EXT_shader_float8` + `shaderFloat8CooperativeMatrix`）。
- 显存里已是 E4M3、数学上也是精确 E4M3、却解包成 FP16 WMMA 的路径：`up_w` / `mrg_w`、W1 `FP8QV`、`DEC_UP` 等走 `v_wmma_f32_16x16x16_fp8_fp8`。
- 门禁：Legacy `max_ulp_diff == 0`；1080p 与 4K 目标算子及端到端同进程 3 轮 A/B 不慢于关闭对照（实际收益以端到端实测为准，不按算子耗时线性相加）。

### 阶段 3：MLP / `wout` 真 FP8 + 可选 `hist_` 压缩

- `fc1`/`fc2`/`wout`：激活侧饱和转 E4M3，**有损，单独门禁**，不与阶段 2 绑成一次提交。
- 可选：`hist_[0..1]` → `R10G10B10A2_UNORM`（4K 约 −149 MB），与 FP8 解耦。

### 每阶段通用门禁

1. 正确性：`test_asi_units`；Legacy 逐位一致；有损阶段另报 PSNR。
2. 算子耗时：1080p + 4K；变慢项不得默认启用。
3. 端到端：`openrr_bench` Steady/DRS + VRAM；证据日志带 git SHA 与着色器特征标签。
4. README 矩阵只写有日志支撑的数字。

---

## 4. 已归档基线对照表（后续测试只需单跑新版 `--runtime`，直接对照下表）

硬件：**AMD Radeon RX 9070 XT（RDNA4，gfx1201）**，驱动 **26.10.1 (LLPC)**，`nvngx_dlssd.dll` **310.9.1.0**。
完整原始日志保存在 [`docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log`](test-evidence/rx9070xt-d3d12-validation-2026-11-08.log)。

### 4.1 端到端同进程 3 轮 A/B 基线数据（`openrr_bench.exe`，`--baseline-runtime` = `d6f70fb` ASI，32 帧预热 + 120 帧稳态 + 64 帧 DRS）

| 档位 (`Render → Output`) | 版本 / 模式 | Feature 显存 | 进程显存增量 | 稳态 `mean` / `median` / `p95` (ms, 3 轮中位) | DRS `mean` / `median` / `p95` / `max` (ms, 3 轮中位) |
|---|---|---|---|---|---|
| **1080p Quality**<br>(`1280×720 → 1920×1080`) | `openrr-amd.asi @ d6f70fb` Fast | `252.0 MB` | `275.8 MB` | `3.182` / `3.171` / `3.263` | `2.821` / `2.976` / `3.369` / `6.660` |
| | **`openrr-amd.asi @ Baseline (B0+B1+B2)` Fast** | **`251.0 MB`** | **`273.5 MB`** | **`3.132` / `3.124` / `3.194`** | **`2.791` / `2.979` / `3.295` / `3.516`** |
| **1440p Quality**<br>(`1706×960 → 2560×1440`) | `openrr-amd.asi @ d6f70fb` Fast | `438.0 MB` | `459.3 MB` | `5.456` / `5.451` / `5.529` | `4.780` / `5.113` / `5.523` / `6.035` |
| | **`openrr-amd.asi @ Baseline (B0+B1+B2)` Fast** | **`435.0 MB`** | **`456.5 MB`** | **`5.405` / `5.391` / `5.496`** | **`4.715` / `5.056` / `5.423` / `5.871`** |
| **4K Quality**<br>(`2560×1440 → 3840×2160`) | `openrr-amd.asi @ d6f70fb` Legacy | `966.0 MB` | `987.5 MB` | `13.095` / `13.089` / `13.195` | `11.464` / `12.385` / `13.225` / `16.564` |
| | `openrr-amd.asi @ Baseline (B0+B1+B2)` Legacy | `960.0 MB` | `980.8 MB` | `13.033` / `13.039` / `13.132` | `11.391` / `12.157` / `13.150` / `13.825` |
| | `openrr-amd.asi @ d6f70fb` Fast | `966.0 MB` | `989.5 MB` | `11.968` / `11.941` / `12.076` | `10.459` / `11.271` / `12.015` / `15.847` |
| | **`openrr-amd.asi @ Baseline (B0+B1+B2)` Fast** | **`960.0 MB`** | **`982.8 MB`** | **`11.830` / `11.812` / `11.933`** | **`10.304` / `11.121` / `11.886` / `15.916`** |

### 4.2 逐算子 Top-8 耗时基线（`test_asi_units.exe` `set_stamps(true)`，22 dispatches = 21 net + 1 flow `clear_range`）

| 排名 | 1080p Quality (`sum = 3.15 ms`, `net21_sum = 3.15 ms`) | 4K Quality (`sum = 11.85 ms`, `net21_sum = 11.85 ms`) |
|---|---|---|
| **#1** | `enc0_stage_32_64_f_lt.spv` — **`375.2 us`** (`11.9%`) | `enc0_stage_32_64_f_lt.spv` — **`1487.6 us`** (`12.5%`) |
| **#2** | `dec_up_32_64_64_f_lt.spv` — **`333.3 us`** (`10.6%`) | `post_f_lt.spv` — **`1389.2 us`** (`11.7%`) |
| **#3** | `post_f_lt.spv` — **`327.0 us`** (`10.4%`) | `dec_up_32_64_64_f_lt.spv` — **`1331.8 us`** (`11.2%`) |
| **#4** | `dec_head_32_64_f_lt.spv` — **`312.5 us`** (`9.9%`) | `dec_head_32_64_f_lt.spv` — **`1296.6 us`** (`10.9%`) |
| **#5** | `enc_stage_64_64_96_f_lt.spv` — **`279.9 us`** (`8.9%`) | `enc_stage_64_64_96_f_lt.spv` — **`1023.0 us`** (`8.6%`) |
| **#6** | `dec_up_64_64_96_f_lt.spv` — **`233.5 us`** (`7.4%`) | `dec_up_64_64_96_f_lt.spv` — **`861.9 us`** (`7.3%`) |
| **#7** | `dec_dfn_64_64_f_lt.spv` — **`202.5 us`** (`6.4%`) | `enc0_attr_f_lt.spv` — **`849.9 us`** (`7.2%`) |
| **#8** | `enc0_attr_f_lt.spv` — **`176.4 us`** (`5.6%`) | `dec_dfn_64_64_f_lt.spv` — **`776.4 us`** (`6.5%`) |

### 4.3 后续单跑新版命令（无需再传 `--baseline-runtime`，耗时减半）

```powershell
# 1080p Quality 单跑新版：
.\build\asi\openrr_bench.exe --runtime build\asi\stage\plugins\openrr-amd.asi --data-dir build\asi\stage\openrr-amd --rw 1280 --rh 720 --ow 1920 --oh 1080 --fast-mode 1

# 1440p Quality 单跑新版：
.\build\asi\openrr_bench.exe --runtime build\asi\stage\plugins\openrr-amd.asi --data-dir build\asi\stage\openrr-amd --fast-mode 1

# 4K Quality 单跑新版：
.\build\asi\openrr_bench.exe --runtime build\asi\stage\plugins\openrr-amd.asi --data-dir build\asi\stage\openrr-amd --rw 2560 --rh 1440 --ow 3840 --oh 2160 --fast-mode 1
```
