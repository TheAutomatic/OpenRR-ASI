# Mesa 26.3.0（drirc 方案重捕）与 27.0-devel 离线 ACO 对照测试结果

在 RX 9070 XT（gfx1201、wave32）上，对 OpenRR Windows D3D12/PAL 离线记录管线对比 26.3.0-drirc（基线：26.3.0 源码 + `radv_enable_custom_border_on_compute_queue=true` 重捕，无 cs_state）与 Mesa 27.0-devel（main 固定 commit）离线生成的 ACO 记录。同一 runtime、同一 SPIR-V、同一权重、同一驱动，仅记录集不同。所有数值为实测。

**结论：27.0-devel（main `f2ccbe6b…`）四项正确性门禁与 26.3.0-drirc 全部逐位一致；稳态 1080p/4K 均 6/6 轮同向小幅更快（中位 -0.83% / -0.54%），DRS 1080p -1.06%（5/6）、4K 持平（-0.07%，3/6）；机器码总量 +1.39%（相对 26.2.4 仍 -6.58%），VGPR/scratch/ABI 零变化，显存零变化。技术上门禁全过、适配成本为零（补丁仅行号漂移），但 27.0 为开发版 pin 且收益 <1%。建议：生产记录集维持 26.3.0 并切换到本轮已验证的 drirc 方案（撤销 cs_state 运行时适配），27.0 等 27.0.0 正式 tag 后按同一流程升级（详见 §11）。**

## 0. 版本身份

| 项 | 值 |
|---|---|
| 基线 | Mesa 26.3.0 release tarball（SHA256 `91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a`），同一份打过捕获补丁的 WSL 源码树，以 `radv_enable_custom_border_on_compute_queue=true`（drirc）**重捕获**（"26.3.0-drirc"），radv 二进制与原 26.3.0 工具链相同（`7fc443f3…`），103 条记录中 95 条与原 26.3.0 记录逐位一致、8 条 sampler 内核少了 cs_state（见 §2） |
| 候选 | **Mesa 27.0 开发版（非正式、非 rc）**：main 分支 commit `f2ccbe6bf47e602a446998a0d8454f9c73cf6186`（`27.0.0-devel`；author 2026-10-23T18:35:04-07:00，committer 2026-11-11T01:44:27Z，"intel: Exclude helper lanes for optimized divergent atomics"）。发布时无 27.0.0/rc tarball（最新 tag 为 `mesa-26.3.0`，2026-11-09），按任务规则固定 main commit。来源校验用完整 hash，不用版本字符串 |
| 27.0 构建 | WSL `~/work/openrr-mesa-270-comparison/{mesa,build}`；meson release，`-Dvulkan-drivers=amd -Dtools=drm-shim`（与 26.2.4/26.3.0 相同参数）；依赖复用 `~/src/mesa-deps`，**无依赖升级**；radv `eb4c543a9d724e33396cf9e28ccc1cad6e62a4682d2815e5051f4aeb5337f9ea` |
| 捕获参数 | `--precision global --matrix-loads vector8 --cmat-address-shaders dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt`；输入 = 90 runtime shader（baseline-data/shaders）+ 23 occ/b2 变体（mesa-aco-0101 `build/asi-delivery-candidate`，只读）；两侧 key 集 = 基线 manifest 103 keys，0 failures |
| cs_state 方案 | 两套捕获均设 `DRIRC_CONFIGDIR` → `radv_enable_custom_border_on_compute_queue=true`，写进各自 toolchain.json 并折入 manifest fingerprint。依据：`radv_shader_args.c` `radv_cs_needs_state_sgpr` 仅当该选项为 false 且 shader 使用 sampler 时追加 `AC_UD_CS_STATE`；**27.0 该函数与 26.3 逐字一致**（431 行），两个调用点（RT prolog/compute）结构相同。cs_state 只控制 compute 队列自定义边框色替换，OpenRR 不使用自定义边框色 |
| runtime | 新工作树重建共享 runtime：`openrr-trace.asi` `2c76f2c9446109cd…`、`openrr_bench.exe` `7d4f0788ae6db5d1…`、`test_aco_units.exe` `4870efb06b9d0443…`；来源校验接受 26.2.4 commit `96cb43…`、`mesa-26.3.0`（tarball 身份；cs_state 推测解析仅对此 lineage 开启）与 27.0 完整 commit hash（严格解析，所有 user SGPR 必须被认领） |
| 权重/驱动 | `nvngx_dlssd.dll`（310.9.1.0）SHA256 `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80`（与 26.3 报告同 DLL）；Adrenalin 26.10.1 |

输入覆盖核验：26.3.0-drirc 与 27.0 的 manifest key 集均与基线 manifest 完全一致（103/103，无缺失/多余）。

## 1. 适配清单（相对 26.3 工作树）

1. **测试执行器修复**（`asi/tests/run_aco_screen.ps1`）：照搬 26.3 修复（文件重定向 + 局部 EAP=Continue，每轮 console.log + console-stderr.log）。
2. **来源校验**（`gpu/src/d3d12.cpp`）：allowlist 增加 27.0 完整 commit hash；`Record::parse` 的 cs_state 推测解析仍仅对 `mesa_commit == "mesa-26.3.0"` 开启（原 26.3.0 数据集保持可加载，供交叉验证）；27.0 manifest 一律严格解析。
3. **cs_state 改为捕获期 drirc 方案（阶段 0）**：26.3.0 重捕（26.3.0-drirc）与 27.0 捕获从一开始就设 `radv_enable_custom_border_on_compute_queue=true`；不再引入新的运行时推测。原 26.3.0 记录（mesa-263-orig 副本）在新 runtime 下仍可运行以完成下述交叉验证。
4. **补丁适配**：`prepare_capture.py` 在 27.0 上全锚点通过，capture.patch 与 26.3 适配版**仅 hunk 行号漂移、内容零变化**：radv_shader.c 555→557、3517→3642、3570→3697（上游在其前新增约 125 行）；radv_descriptor_set.c 244→240；aco_assembler.cpp 1232→1233、1885→1886；radv_device.c 1214→1213。`global-address.patch` 与 26.3 版**逐字节一致**（SHA256 `786e97c9…`）。divergent push load 段自 26.3 起已上游实现，继续跳过；其余补丁段在 27.0 无上游覆盖。capture.patch(270) SHA256 `f3350d149bccd355…`。
5. **捕获/构建脚本**：新增 `asi/build/mesa/build_capture_270.sh`（git 源、pinned commit、drirc conf 生成、toolchain.json 记录 commit 日期与 drirc）。

## 2. 阶段 0：26.3.0-drirc 与原 26.3.0 记录等价性

- 记录对照（`mesa-263-drirc/tools/record-comparison-263drirc-vs-263orig.json`）：95/103 机器码逐位一致；8 条差异即 cs_state 集合（post_f、post_f_lt ×4 内容变体、post_dlt_f、post_dlt_f_lt、探针 selftest_tex——修正 26.3 报告中 "pooled_history2_lt、enc_stage_128_128_160_fl_lt 受影响" 的不精确提法；pooled_history2_lt/enc_stage_128_128_160_fl_lt 的 user_sgprs 在 26.2.4→26.3.0 间本就未变）：每条 user_sgprs −1、exec 共 −1544 B（守卫代码消失），VGPR/scratch/其余元数据零变化。26.2.4→26.3.0 的 SGPR 差集与 26.3.0→26.3.0-drirc 差集**完全相等**（同一集合的两种视角）。
- GPU 交叉验证（共享 runtime，Fast 1080p/4K + Legacy fast_mode=0 1080p/4K）：全部 exact=100%、ulp=0、非有限值=0；Legacy 每侧 20+20 帧读回 **SHA256 全部一致**（`mesa-263-drirc/mesa-263-drirc-x-*-legacy-r1-frame-hashes.txt`）。运行时 cs_state 推测解析（原 26.3.0 记录侧）与严格解析（drirc 侧）在真实 dispatch 中同时得到验证。
- 结论：cs_state 运行时适配（parse 推测 + prologue s_mov）可安全退役；两套 26.3.0 记录在输出上不可区分。

## 3. ABI 与覆盖（27.0 记录）

- **ABI 元数据零变化**：103 条记录的 user_sgprs/set0/push/grid/push_bytes/scratch/shared_bytes/wave_size 与 26.3.0-drirc 完全一致 —— 27.0 未引入新的未认领 user SGPR 或新参数类型，无需 ABI 变通，未触发"查不清即停"分支。
- **ABI 探针**（`test_aco_units.exe OPENRR_ACO_PROBE`）：26.3.0-drirc 4/4 PASS（`abi-probe-263-drirc.log`）、27.0 4/4 PASS（`abi-probe-270-r2.log`）：244 字节 push ABI/spill 指针/inline 常量/workgroup count、lane-varying push 索引（128 输出）、clear_range 纯 BDA 填充、sampled 描述符槽 0/3/7。
- **覆盖检查**：
  - bench trace（`--optional-guides 1 --trace-frame 3`，双侧，`mesa-270/coverage-trace/*/trace.txt`）：每帧 25 个 dispatch **全部 aco=1**，含 pooled_history2_lt、post_f_lt、enc_stage_128_128_160_fl_lt、宽路径 d3d_enc0_attr_f_w_lt + enc0_stage_32_64_f_w_lt。
  - post_dlt_f_lt：bench 恒为 EDIT_OFF，用引擎级驱动工具 `mesa-270/tools/post_dlt_dispatch.cpp`（dispatch_trace_ 在真实 Dispatch 后触发）证明 display-edit 帧建立 display history 后，后续无编辑帧仍调度 post_dlt_f —— 26.3.0-drirc 与 27.0 记录集各自 PASS（`post-dlt-*.log`）。
  - 单元套件（全量）：26.3.0-drirc 与 27.0 各 27 项全 PASS（`units-*-full*.log`），含 D3D12 Direct Engine Fast(_lt+flow)/Legacy(_f+flow)（1080p+4K）、arena/wide 20 帧序列（DOF 帧 10–14）、R10；compiler=ACO 661 次、LLPC 0 次。

## 4. 正确性（GPU 实测，全部门槛通过）

Fast 门槛：PSNR_01≥54 dB、PSNR_TM≥58 dB、p99≤0.006、非有限值=0；Legacy 门槛：逐位一致。

| 配置 | 运行名 | Fast | Legacy 逐位 |
|---|---|---|---|
| 1080p（1280×720→1920×1080） | `mesa-270-1080p-r1` / `mesa-270-1080p-legacy-r1` | exact=100%，ulp=0，p99=max=0，非有限值=0（steady+drs+逐帧） | 20/20 帧 SHA256 一致 |
| 4K（2560×1440→3840×2160） | `mesa-270-4k-r1` / `mesa-270-4k-legacy-r1` | exact=100%，ulp=0，p99=max=0，非有限值=0 | 20/20 帧 SHA256 一致 |

27.0 与 26.3.0-drirc 的四项正确性门禁全部逐位一致（PSNR_01/PSNR_TM 因输出完全相同为 999 dB 满值）。加上阶段 0 的 26.3.0-drirc ≡ 原 26.3.0（≡ 26.2.4，见 26.3 报告），27.0 输出与整条 lineage 传递性逐位一致。

## 5. 稳态与 DRS 耗时（Long：6 对交替、48 warmup / 240 steady / 64 DRS，trace/stamps/逐帧读回关闭，align-engine-phase 开启；测速期间无并行 CPU 重负载）

按 `order.json` 角色映射配对，27.0 相对 26.3.0-drirc 的 GPU 时间变化（负值=27.0 更快）：

| 指标 | 1080p | 4K |
|---|---|---|
| 稳态中位配对变化（6 轮范围） | **-0.83%（-0.51…-1.24，6/6 同向更快）** | **-0.54%（-0.45…-0.72，6/6 同向更快）** |
| 稳态中位绝对值 | 3.499 → 3.472 ms | 12.917 → 12.846 ms |
| 稳态 P95（干净轮） | 3.583 → 3.549 ms | 13.098 → 13.011 ms |
| DRS 中位配对变化 | **-1.06%（-0.69…-1.33，5/6 更快，1 轮 +0.42）** | **-0.07%（-2.11…+1.18，3/6 更快，持平）** |
| DRS 中位绝对值 | 3.335 → 3.299 ms | 12.140 → 12.116 ms |
| DRS P95 | 3.716 → 3.662 ms | 13.147 → 13.004 ms |
| DRS 单轮最大（每版 3 个样本） | 3.89–8.67 vs 3.89–8.63 ms（同量级） | 13.75–13.86 vs 13.64–17.88 ms（270 有一次 17.9 ms 尾刺） |

轮间噪声备注：1080p 第 1–2 轮两版 steady P95 同时抬到 ~8.2 ms（环境瞬态，双侧同受影响，配对中位数不受污染）；干净轮 P95 两版相当。4K 各轮干净（P95 13.0–13.2 ms），仅 round-2 candidate 段有一次 17.9 ms DRS 尾刺。Long 各轮 parity 均 exact=100%。逐轮数据：`mesa-270/tools/mesa-270-{1080p,4k}-long-r1.json`。

## 6. 显存

| 指标 | 1080p | 4K |
|---|---|---|
| runtime 自报 feature VRAM | **207 MB = 207 MB（各 3 轮恒定）** | **789 MB = 789 MB（各 3 轮恒定）** |
| DXGI 本地显存进程增量（candidate 段） | 229.1 MB | 810.3 MB |

与 26.3 报告相同：summary.json 仅 candidate 段输出显存，6 轮交替 + 角色映射覆盖两版；无独立 DRS 显存通道（稳态值已足够回答"零显存变化"）。
## 7. 记录对照（`mesa-270/tools/record-comparison-270-vs-263drirc.json`）

| 项 | 26.3.0-drirc | 27.0-devel |
|---|---|---|
| 机器码不同的记录 | — | 102/103（1 条逐位相同） |
| 代码总量（exec bytes） | 2,166,248 | 2,196,324（**+1.39%**） |
| VGPR 总和 | 16,520 | 16,520（持平；**无任何内核跨越 192/216/240/256 台阶**） |
| scratch | 0 | 0 |
| user SGPR 元数据 | — | 无变化（见 §3） |
| 其余 ABI 元数据 | — | 无变化 |

Top-6 大核（_f_lt 变体，27.0 vs 26.3.0-drirc）：

| 内核 | exec | 变化 | VGPR |
|---|---|---|---|
| enc0_stage_32_64_f_lt | 23300 → 23168 | −0.6% | 192→192 |
| post_f_lt（4 个内容变体） | 20812–22376 → 21728–22984 | +3.7…+4.4% | 120→120 |
| dec_up_32_64_64_f_lt | 24772/24912 → 24748/24892 | −0.1% | 256/216 持平 |
| dec_head_32_64_f_lt | 21244 → 21436 | +0.9% | 192→192 |
| enc_stage_64_64_96_f_lt | 30956–31272 → 31300–31620 | +0.4…+1.1% | 256/240 持平 |
| dec_up_64_64_96_f_lt | 25744/25840 → 26060/26168 | +0.9…+1.6% | 240→240 |

代码量增幅集中在 post 系（sampler 描述符路径 +4%）与 d3d_enc0_attr_f_w（+3.1%）；编码/解码主体 ±1%。与 26.3.0→26.2.4 的 -7.81% 相比，27.0 的代码生成小幅回涨，无占用率结构变化。

对照 26.2.4 全链（`record-comparison-270-vs-2624.json`）：27.0 相对 26.2.4 代码总量 **-6.58%**（2,351,340 → 2,196,324）、VGPR 16,584 → 16,520、scratch 3×8 B → 0；26.3.0 报告中 256→240 的 4 条内核（dec_up_64_64_96_f[_lt]、enc_stage_64_64_96_f[_lt]）在 27.0 保持 240。

### 7.1 阶段 2：Stage 1.5 保留项在 27.0 工具链上的复核（离线，胜出版本）

先以 `build_shaders.py` 复现官方 spv（两个目标内核 FNV key 均与 manifest 精确一致，工具链可复现），再做单因子 off 捕获（`mesa-270/tools/stage2/`）：

| 开关 | 内核 | exec（off vs 现行 on） | VGPR | scratch |
|---|---|---|---|---|
| cmat-address off | dec_dfn_96_128_fl_lt | 24200 vs 22696（**+6.6%**） | 256=256 | 0=0 |
| cmat-address off | enc_stage_128_128_fl_lt | 34164 vs 32016（**+6.7%**） | 240=240 | 0=0 |
| enc128 DontUnroll off（cmat 保持 on） | enc_stage_128_128_fl_lt | 47072 vs 32016（**+47.0%**） | 240=240 | 0=0 |

结论：两项保留项在 27.0 的成本模型下依然成立——关闭任一项都只有代码劣势（寄存器/占用结构不变），无任何离线指标改善，按有界协议不再做短测。DontUnroll 与 cmat-address 的相互作用与源码注释一致（去掉提示后 ACO 全展开越过 bounded cmat 降址的成本上限）。不扩展到其他循环或开关。

## 8. 失败与诊断记录（全部保留）

- `mesa-263-drirc-x-1080p-r1`（首次交叉验证）：candidate create 失败 "cannot open …data\shaders\ingest.spv" —— 数据目录需同时含 `shaders/`；补齐后 r2 通过。经验固化：所有 data 目录均放置 runtime shader 集。
- 27.0 ABI 探针首轮失败 "ACO manifest provenance mismatch"：d3d12.cpp 的 27.0 hash 编辑因相对路径误落进 26.3 只读工作树（已精确还原并复核 git diff 与原始一致），270 工作树补上编辑并重建 runtime 后 4/4 PASS。全程只读目录最终状态未受影响。
- units 全量首跑失败 ×3：`data/shaders` 缺失（补齐）；`build/asi`（Downloads 旧副本）中 stage/b2_attr/occ 目录的 spv 字节与基线 manifest 不匹配（hash 不在 key 集）——同步为交付候选版本后通过；R10（默认目录发现用例）因 exe 目录与 CWD 的 bdir 解析不一致失败——在 `build/aco-runtime-trace` 旁复刻交付布局（stage + 输入目录）后通过。
- WSL git 访问 gitlab 失败：`~/.gitconfig` 残留旧网关代理 `172.17.64.1:7897`（git 配置优先于环境变量）；经用户提供的代理 `172.21.16.1:7897` + Windows 侧克隆转接解决。未修改用户 gitconfig。
- 无 GPU 驱动 TDR 事件；除上述外无硬件故障。

## 9. 复现命令

```bash
# 27.0 工具链（WSL，源码 git clone 固定 f2ccbe6b…，捕获补丁由脚本幂等施加）
bash asi/build/mesa/build_capture_270.sh          # meson+ninja + aco_capture + toolchain.json + drirc conf
# 26.3.0-drirc 工具链：复用 26.3 源码/构建 + build/mesa-comparison/mesa-263-drirc-toolchain/toolchain.json（含 drirc 字段）

# 记录捕获（WSL；DRIRC_CONFIGDIR 指向对应 drirc 目录；occ/b2 输入用原交付候选目录，只读）
python3 asi/build/build_aco_records.py --toolchain <toolchain.json> --capture <aco_capture> \
  --capture-dir <capture-diagnostics> --out <data>/aco --precision global --matrix-loads vector8 \
  build/mesa-comparison/baseline-data/shaders \
  <mesa-aco-0101>/build/asi-delivery-candidate/{b2_attr_rcp,b2_attr_same,b2_base,b2_c64_far3,b2_post_exp,b2_post_log,occ_nopad,occ_pad16k} \
  --cmat-address-shaders dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt
```

```powershell
# 共享 runtime（270 工作树重建）：asi/build/build_runtime_trace.sh（WSL mingw）
# 正确性与 Long（26.3.0-drirc 为 baseline，27.0 为 candidate）
& asi/tests/run_aco_screen.ps1 -Name mesa-270-1080p-r1 -Baseline build/mesa-comparison/mesa-263-drirc/data `
  -Candidate build/mesa-comparison/mesa-270 -PerDataRecords -Stage Correctness   # 4K 加 -RenderWidth 2560 …
& asi/tests/run_aco_screen.ps1 -Name mesa-270-1080p-long-r1 … -Stage Long
# Legacy：直接调 openrr_bench.exe（--fast-mode 0，参数布局同上），逐帧 bin 求 SHA256
# ABI 探针 / units / 覆盖 / post_dlt 证据：
#   OPENRR_ACO_PROBE=1 + ONR_ACO_DIR=<records> test_aco_units.exe
#   units 全量 + arena/wide；bench --optional-guides 1 --trace-frame 3；tools/post_dlt_dispatch.exe
# 离线对照与汇总
python build/mesa-comparison/mesa-270/tools/compare_mesa_records.py <a.json> <b.json> [out.json]
python build/mesa-comparison/mesa-270/tools/summarize_mesa270_long.py mesa-270-1080p-long-r1 mesa-270-4k-long-r1
```

## 10. 原始证据位置

- 运行原始数据：`build/aco-runtime-trace/runs-next-optimization-2026-11-10/mesa-263-drirc-x-*`、`mesa-270-*`；Legacy/交叉验证帧哈希：`build/mesa-comparison/mesa-{263-drirc,270}/*-frame-hashes.txt`。
- 捕获诊断：`build/mesa-comparison/mesa-{263-drirc,270}/capture-diagnostics/`；探针记录：`mesa-{263-drirc,270}/abi-probe/` + 各 `abi-probe-*.log`、`units-*-full*.log`；覆盖 trace：`mesa-270/coverage-trace/`；post_dlt 证据：`mesa-270/tools/post_dlt_dispatch.cpp` + `post-dlt-*.log`。
- 记录对照：`mesa-270/tools/record-comparison-270-vs-263drirc.json`、`mesa-263-drirc/tools/record-comparison-263drirc-vs-263orig.json`。
- 清单：`build/mesa-comparison/mesa-270/inventory/inventory.json`；进度留痕：`build/mesa-comparison/omp-progress.md`。
- 未动 `mesa-aco-0101`、`mesa-263-comparison` 两个只读工作树（26.3 工作树曾因相对路径编辑被短暂污染，已还原并复核）；`capture.patch`/`global-address.patch`/`prepare_capture.py`/`run_aco_screen.ps1`/`d3d12.cpp`/`rr_pal_binary.hpp`/`build_capture_270.sh` 为本工作树改动，未提交、未推送。

## 11. 建议
**建议：生产记录集维持 26.3.0，立即切换到本轮已验证的 drirc 方案；27.0-devel 暂不采用，待 27.0.0 正式 tag 后按本报告流程升级。**理由：

1. **收益小于风险**：27.0 的全部优势是稳态中位 -0.83%/-0.54%（<1%），DRS 4K 持平（-0.07%，3/6 同向）且有一次 17.9 ms 尾刺；代价是开发版身份——`f2ccbe6b…` 是 main 上的开发 commit，非 rc、非正式版，27.0.0 发布前上游仍会变，每次 rebase pin 都要重跑完整门禁（本轮全流程约一个工作日）。
2. **维护成本本轮确认为零，但每版仍需一次完整复核**：capture.patch 仅行号漂移、global-address.patch 逐字节相同、ABI 元数据零变化——27.0 没有破坏任何适配；但"零适配"是本轮实测结论，不是对未来 commit 的承诺（26.2.4→26.3.0 就新增过 cs_state）。
3. **26.3.0-drirc 是无成本的即时改进**：本轮证明它与现行 26.3.0 记录输出逐位等价、代码还小 1.4 KB/8 内核，并让 runtime 摆脱 cs_state 推测解析。

配置差异说明（按任务要求注明）：现行生产 **26.3.0 基线带 cs_state 运行时适配**（parse 推测 + prologue 常量 0），**27.0（及 26.3.0-drirc）不带**；两者在 8 个 sampler 内核上相差一条 s_mov（运行时 prologue 注入）和一小段描述符条件选择（26.3.0 记录内的守卫代码），实测输出逐位一致，不影响任何结论。

**后续清理方式（按胜出方）：26.3.0 胜出 →**
- 生产按驱动配置项方案重新捕获 26.3.0（即本报告的 mesa-263-drirc 流程：捕获时设 `radv_enable_custom_border_on_compute_queue=true` 并写入 toolchain.json/manifest/指纹）；
- 撤掉 runtime 的 cs_state 推测解析与 prologue 改动（`rr_pal_binary.hpp` 的 allow_cs_state 分支、`d3d12.cpp` 的对应传参），cs_state 相关代码整体删除；
- 来源校验从 `mesa-26.3.0` 版本字符串改为 26.3.0 tarball 的内容指纹（如 radv_sha256 + patch 哈希组合），runtime 只保留 26.2.4 与 26.3.0-drirc 两个 lineage 的严格检查；
- 27.0.0 正式发布后：固定 release tag → 重跑 prepare 脚本（预期仅行号漂移）→ 捕获 → ABI 元数据 diff（零变化则免深度排查）→ 四项正确性门禁 + 一轮 Long 短测即可升级，全程可用本报告脚本复现。

（若后续改判 27.0 胜出：废弃 26.3 的 cs_state 解析和 prologue 改动，runtime 只保留 27.0 一套工具链，26.3.0 lineage 从 allowlist 移除。）

