# Mesa 26.2.4 与 26.3.0 离线 ACO 对照测试结果

在 RX 9070 XT（gfx1201、wave32）上，对 OpenRR Windows D3D12/PAL 离线记录管线对比 Mesa 26.2.4（基线，commit `96cb43121031992b85767f9c1be8f3f48e22b1d2`）与 Mesa 26.3.0（release tarball）离线生成的 ACO 记录。同一 runtime、同一 SPIR-V、同一权重、同一驱动，仅记录集不同。所有数值为实测。

**结论：26.3.0 记录在全部四项正确性门槛（1080p/4K × Fast/Legacy）与 26.2.4 逐位一致；6 轮交替 Long 测量全部同向提速——稳态 1080p -4.17%、4K -4.06%，DRS 1080p -3.44%、4K -4.07%；机器码总量 -7.81%，显存零变化。可升级。**

## 1. 测试条件与样本锁定

| 项 | 值 |
|---|---|
| GPU / 驱动 | AMD Radeon RX 9070 XT（gfx1201），Adrenalin 26.10.1（LLPC 提交路径），无 TDR 事件（System/Display 日志 120 min 内为空） |
| 工作树 | `C:\Users\Kenny\.codex\worktrees\mesa-263-comparison\OpenRR`，分支 `codex/mesa-263-comparison`，基线源码 `934efd9` |
| Mesa 26.3.0 tarball | SHA256 `91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a`（`archive.mesa3d.org/mesa-26.3.0.tar.xz`） |
| 26.3.0 构建 | WSL `~/work/openrr-mesa-263-comparison/{mesa-26.3.0,build}`；meson release，`-Dvulkan-drivers=amd -Dtools=drm-shim`（与 26.2.4 相同参数，依赖复用 `~/src/mesa-deps`）；`radv_sha256=7fc443f30e8abe994f5c9fd1ae4f76cfe753fe04048764a9fc65218b27c539ee` |
| 捕获补丁 | `capture.patch` 26.3.0 适配版 SHA256 `27628e5aa27137fc872094730b30f0b0017ab5163ca823a0832b4313c5c27584`（上游已实现 divergent push load，该补丁段跳过）；`global-address.patch` SHA256 `786e97c384fe2713bc63214d6afa71f19c56e6846672e0a0df543399ea53868a`（锚点内容与 26.2.4 相同，仅 hunk 行号漂移）。26.2.4 原版两补丁已存档于 `build/mesa-comparison/mesa-263/patches/`（哈希与 `baseline-toolchain/toolchain.json` 记录一致：`44b9ab78…`、`3c3672ff…`） |
| 记录 | 103/103（`--precision global --matrix-loads vector8 --cmat-address-shaders dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt`），key 集与基线 manifest 完全一致，0 failures；NRPAL002/FNV/sha256/msgpack 索引全部核验通过（`verify_aco_delivery.py` 校验逻辑） |
| 输入 SPIR-V | 90 个 runtime shader（`baseline-data/shaders`）+ 23 个 occ/b2 变体（取自原交付候选目录，只读），与基线 manifest 的 103 个 key 精确对齐 |
| 权重 | `nvngx_dlssd.dll`（310.9.1.0）SHA256 `4bc7ea5fcb2f32cf…`（完整值见 inventory），两版共用 |
| 比较 runtime | 双侧共用同一重建 runtime（含下述最小适配）：`openrr-trace.asi` SHA256 `ba2f6ad68bb9d185…`、`openrr_bench.exe` `8fa8133847224862…`、`test_aco_units.exe` `e665ab6f607e9e82…`；完整值与原版存档见 `build/mesa-comparison/mesa-263/inventory/inventory.json` 与 `runtime-baseline-copy/` |
| 样本清单 | `build/mesa-comparison/mesa-263/inventory/inventory.json`（记录/shader/EXE/ASI/权重/补丁/tarball 全量 SHA256） |

输入哈希覆盖核验：103 个捕获输入 key = 基线 manifest key，无缺失/多余（`inventory.json` 的 `missing_vs_baseline_manifest`/`extra_vs_baseline_manifest` 均为空）。哨兵适配报告与 26.2.4 同构（`subst` 分布逐形状一致，`constdata=0、pc_relative=0、writes=0` 全部记录）。

## 2. 相对基线的最小适配（均为双侧对称）

1. **provenance 门放宽**（`gpu/src/d3d12.cpp`）：manifest 来源检查接受 `96cb43…`（26.2.4）或 `mesa-26.3.0`（tarball 身份，tarball SHA256 折入 fingerprint）。运行时行为对 26.2.4 记录不变（重建后 baseline-vs-baseline 自检 20/20 帧逐位一致）。
2. **Mesa 26.3 新增 `AC_UD_CS_STATE` user SGPR**：26.3.0 给带 sampler 的 compute 内核追加第 7/14 号 user SGPR（`cs_state`，bit0 = "来自 compute 队列"），8/103 条记录受影响（`post_f_lt`、`pooled_history2_lt`、`post_dlt_f_lt`、`enc_stage_128_128_160_fl_lt` 等 7 条 + 探针 `selftest_tex`）。适配（`gpu/src/rr_pal_binary.hpp`）：parse 将唯一未认领寄存器判为 `cs_state`（仅对 `mesa-26.3.0` manifest 开启）；prologue 在全部入参搬移/加载之后写常量 0 —— bit0=0 使 26.3 的 compute 队列边界色守卫不生效，sampler 描述符与 26.2.4 管线逐位相同（OpenRR 的 sampler 均为 LLPC `vkCreateSampler` 线性/clamp 描述符，不含自定义边界色寄存器类型）。记录格式未变。
3. **测试执行器修复**（`asi/tests/run_aco_screen.ps1`）：原 `2>&1 | Out-File` 在 `$ErrorActionPreference='Stop'` 下会因首条 stderr 行打断 bench（PowerShell 5.1 NativeCommandError 包装伪影，表现为进程提前终止），改为文件重定向 + 局部 EAP=Continue，每轮新增 `console-stderr.log`。

编码与顺序经离线工具核验（`build/mesa-comparison/mesa-263/tools/check_prologue.cpp`，直接编译真实的 `rr_pal_binary.hpp`）：8 条受影响记录的 `s_mov_b32 sN, 0x0` 编码（SOP1 inline 常量 `0x80+v`，依据 `aco_ir.h` `Operand::c32`→`PhysReg{128+v}`）、写入位置（最后一次 PAL 槽读取之后）、splice 元数据（`.user_sgprs`、`user_data_reg_map`、`.user_data_limit`）全部 PASS。

## 3. 正确性（GPU 实测，全部门槛通过）

Fast 门槛：PSNR_01≥54 dB、PSNR_TM≥58 dB、p99≤0.006；Legacy 门槛：逐位一致（max_ulp_diff=0）。

| 配置 | 运行名 | Fast 门槛 | Legacy 逐位 |
|---|---|---|---|
| 1080p（1280×720→1920×1080） | `mesa-263-1080p-r5` / `mesa-263-1080p-legacy-r1` | exact=100%，ulp=0，p99=max=0，非有限值=0（steady+drs） | 20/20 帧 SHA256 一致（`legacy-1080p-frame-hashes.txt`） |
| 4K（2560×1440→3840×2160） | `mesa-263-4k-r1` / `mesa-263-4k-legacy-r1` | exact=100%，ulp=0，p99=max=0 | 20/20 帧 SHA256 一致（`legacy-4k-frame-hashes.txt`） |

ABI 探针（`test_aco_units.exe OPENRR_ACO_PROBE`，26.3.0 记录）：244 字节 push ABI/spill 指针/inline 常量/workgroup count、lane-varying push 索引（128 输出）、clear_range 纯 BDA 填充、sampled 描述符槽 0/3/7 全部 PASS（`abi-probe/abi-probe-263-round3.log`）。双 sentinel 一致性与 m0/常量数据/PC 相对检查由捕获侧完成（见 §1 末段）。

## 4. 稳态与 DRS 耗时（Long：6 对交替、48 warmup / 240 steady / 64 DRS，trace/stamps/逐帧读回关闭，`align-engine-phase` 开启）

按 `order.json` 角色映射配对，26.3.0 相对 26.2.4 的 GPU 时间变化（负值=更快）：

| 指标 | 1080p | 4K |
|---|---|---|
| 稳态中位配对变化（6 轮范围） | **-4.17%（-4.01…-4.47，6/6 同向）** | **-4.06%（-3.91…-4.18，6/6 同向）** |
| 稳态中位绝对值 | 3.655 → 3.504 ms | 13.415 → 12.873 ms |
| 稳态 P95 | 3.754 → 3.549 ms | 13.561 → 13.014 ms |
| DRS 中位配对变化（6 轮范围） | **-3.44%（-2.99…-4.14，6/6 同向）** | **-4.07%（-2.77…-4.73，6/6 同向）** |
| DRS 中位绝对值 | 3.423 → 3.305 ms | 12.623 → 12.092 ms |
| DRS P95 | 3.907 → 3.730 ms | 13.601 → 13.050 ms |
| DRS 单轮最大（每版 3 个样本） | 4.09–7.63 vs 7.29–8.45 ms（同量级，26.3 最大值略高） | 14.35–18.37 vs 13.63–15.26 ms（26.3 更好） |

逐轮数据与统计：`build/mesa-comparison/mesa-263/tools/mesa-263-1080p-long-r1.json`、`mesa-263-4k-long-r1.json`。6 轮交替使两版各自担任过 baseline/candidate，candidate 侧才输出的 p95/max/显存因此两版均被覆盖。Long 各轮 parity 亦为 exact=100%。

## 5. 显存

| 指标 | 1080p | 4K |
|---|---|---|
| runtime 自报 feature VRAM | **207 MB = 207 MB（两版逐轮完全一致）** | **789 MB = 789 MB（两版逐轮完全一致）** |
| DXGI 本地显存进程增量（candidate 段，6 轮恒定） | 229.086 MB | 810.336 MB |

现有 `summary.json` 只在 candidate 段输出显存；6 轮交替 + 角色映射已覆盖两版（§4）。无逐相（DRS 独立）显存样本通道；如需 DRS 时刻显存需给 bench 加对称 JSON 输出，本轮未做（稳态值已足够回答"零显存变化"）。

## 6. 记录对照（`build/mesa-comparison/mesa-263/tools/record-comparison.json`）

| 项 | 26.2.4 | 26.3.0 |
|---|---|---|
| 机器码不同的记录 | — | 100/103（3 条逐位相同） |
| 代码总量（exec bytes） | 2,351,340 | 2,167,792（**-7.81%**） |
| VGPR 总和 | 16,584 | 16,520（-0.39%）；4 条内核 256→240（`dec_up_64_64_96_f[_lt]`、`enc_stage_64_64_96_f[_lt]`），占用率提升 |
| scratch | 3 条记录 ×8B（`enc_stage_96_128_128_*`） | **0（全部消除）** |
| user SGPR 元数据 | — | 8 条 sampler 内核 +1（cs_state，见 §2.2） |
| 其余 ABI 元数据（set0/push/grid/shared/push_bytes） | — | 无变化 |

提速最大的内核与代码收缩一致：`enc_stage_96_128_128_*`（-24.0% 代码，scratch 归零）、`enc_stage_64_64_96_f_lt`、`dec_up_64_64_96_f_lt`。VGPR 256→240 使每 SIMD 波占用从受限档位释放，与 -4% 端到端帧时间量级吻合（编码/解码段主导）。

## 7. 失败与诊断记录（全部保留）

- `mesa-263-1080p`（首跑，PS 管道伪影终止）、`mesa-263-1080p-r2/r3/r4`（同因）、`diag-1080p`、`diag-llpc-candidate`：保留于 `build/aco-runtime-trace/runs-next-optimization-2026-11-10/`。判别实验结论见 §2.3。
- 探针 round1（`abi-probe-263.log`）：记录解析期 `unhandled adapter argument` → 发现 cs_state。round2（`abi-probe-263-fixed.log`）：prologue 两处缺陷（s1 寄存器误当立即数、写入过早污染第 5 个 inline 常量源）→ 0x887a0006，离线定位后修复。round3（`abi-probe-263-round3.log`）：4/4 PASS。
- 无 GPU 驱动 TDR 事件；失败均非硬件故障。

## 8. 复现命令

```bash
# 26.3.0 工具链（WSL，源码已按 handoff 解压并打过捕获补丁）
bash asi/build/mesa/build_capture_263.sh          # meson+ninja + aco_capture + toolchain.json
# 记录捕获（WSL；occ/b2 输入用原交付候选目录以保持 key 一致）
python3 asi/build/build_aco_records.py --toolchain build/mesa-comparison/mesa-263-toolchain/toolchain.json \
  --capture build/mesa-comparison/mesa-263-toolchain/aco_capture \
  --capture-dir build/mesa-comparison/mesa-263/capture-diagnostics \
  --out build/mesa-comparison/mesa-263/data/aco --precision global --matrix-loads vector8 \
  build/mesa-comparison/baseline-data/shaders <原交付候选>/occ_nopad …/b2_c64_far3 \
  --cmat-address-shaders dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt
```

```powershell
# 比较 runtime 已在 build/aco-runtime-trace（两版共用）；屏幕与 Long
& asi/tests/run_aco_screen.ps1 -Name <新名> -Baseline build/mesa-comparison/baseline-data `
  -Candidate build/mesa-comparison/mesa-263 -PerDataRecords -Stage Correctness   # Fast；Legacy 用 bench --fast-mode 0（见 §3 参数）
& asi/tests/run_aco_screen.ps1 -Name <新名> … -Stage Long                          # 4K 加 -RenderWidth 2560 -RenderHeight 1440 -OutputWidth 3840 -OutputHeight 2160
# 离线核验与汇总
python build/mesa-comparison/mesa-263/tools/check_prologue.cpp            # 需 g++ 编译（WSL）
python build/mesa-comparison/mesa-263/tools/compare_mesa_records.py
python build/mesa-comparison/mesa-263/tools/summarize_mesa263_long.py mesa-263-1080p-long-r1 mesa-263-4k-long-r1
```

## 9. 原始证据位置

- 运行原始数据（summary/order/console/角色日志/帧读回）：`build/aco-runtime-trace/runs-next-optimization-2026-11-10/mesa-263-*`；小文件归档 + SHA256 清单：`build/mesa-comparison/mesa-263/archive/`（`archive-sha256.json`）。
- 捕获诊断（每 shader 双 sentinel .code/.json/.layouts/.log）：`build/mesa-comparison/mesa-263/capture-diagnostics/`；反汇编样本：`post_f_lt-263-asm.log`、`abi-probe/selftest_tex-263-asm.log`。
- 离线校验工具：`build/mesa-comparison/mesa-263/tools/`（check_prologue、compare_mesa_records、summarize_mesa263_long、dump_template、query 脚本）。
- 进度留痕：`build/mesa-comparison/omp-progress.md`。
- 未动原 ACO 工作树（`mesa-aco-0101`）与基线快照；`capture.patch`/`prepare_capture.py`/`global-address.patch`/`gpu/src/d3d12.cpp`/`gpu/src/rr_pal_binary.hpp`/`asi/tests/run_aco_screen.ps1`/`asi/build/mesa/build_capture_263.sh` 为本工作树内改动，未提交、未推送。
