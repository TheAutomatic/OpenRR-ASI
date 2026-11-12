# OpenRR-ASI (`openrr-amd.asi`) 测试证据归档目录

本目录统一存放所有实机单元测试（`test_asi_units.exe`）与同进程 A/B 基准测试（`openrr_bench.exe`）的留档记录。**严禁在 `build/asi/` 下散落保留临时 `bench-*` 目录。**

## 目录结构与规范

1. **当前阶段主证据**：
   - [`rx9070xt-d3d12-validation-2026-11-08.log`](rx9070xt-d3d12-validation-2026-11-08.log)：当前分支最新通过门禁的完整实机测试与 3 轮同进程 A/B 对照报告。
   - [`game-baseline-2026-11-09.md`](game-baseline-2026-11-09.md)：三款实机游戏（《赛博朋克 2077》、《影之刃零 PBZ》、《红色沙漠 Crimson Desert》）初始基线运行报告、显存占用与帧耗时表格（原始游戏日志本地归档于 `game-logs/2026-11-09-initial-baseline/`，通过 `.gitignore` 忽略不入库）。

1b. **v0.1.5 测试证据与勘误总索引（2026-11-12 更新，按时间顺序）**：
   - [`v015-anchor2-rtx5090-v1-results.md`](v015-anchor2-rtx5090-v1-results.md) / [`v015-anchor2-rtx5090-v2-results.md`](v015-anchor2-rtx5090-v2-results.md)：锚点 #2 OpenRR vs RTX 5090 `nvngx_dlssd.dll 310.9.1.0` 首轮对账（v1 全量 + v2_motion 全 21 用例）。**⚠ 数值已被相位对齐勘误整体更新**（蓝噪声 `frame_%8` 与窗口 `frame_&1` 错拍系统性压低 PSNR 0.5~2.0 dB），真值见 `anchor2-phase-aligned/`。
   - [`anchor2-phase-aligned/`](anchor2-phase-aligned/)：**当前画质真值**。相位对齐后的 v1/v2 全量 per-frame CSV 与哈希门禁（legacy / fast-llpc / fast-aco 三档）。对齐后 fast-llpc 与 fast-aco 收敛至 ≤0.02 dB。
   - [`v015-stage2-vgpr-xin8-results.md`](v015-stage2-vgpr-xin8-results.md) / [`v015-stage2-day-results.md`](v015-stage2-day-results.md) / [`v015-stage2-overnight-results.md`](v015-stage2-overnight-results.md)：Stage 2 VGPR/XIN8 占用率攻坚系列，结论收敛于 256-VGPR 钉死的物理根因（128 KiB/WGP LDS 上限，`floor(131072/lds)×(wg/32)/4`），occupancy 旋钮路线作废。
   - [`v015-stage2-radv-cmat-results.md`](v015-stage2-radv-cmat-results.md)：Stage 2 首轮（`80fd0d7`）。**⚠ 勘误**：Mesa 补丁被 `build_capture_263.sh` 旧守卫静默跳过，II/III 档为空跑（I 档 fp16 路径数据有效）。
   - [`v015-stage2-radv-cmat-v2-results.md`](v015-stage2-radv-cmat-v2-results.md)
   - [`v015-integration-results.md`](v015-integration-results.md) / [`v015-interface-parity-results.md`](v015-interface-parity-results.md)：v0.1.5 集成与接口奇偶校验报告（补录索引）。
   - [`v015-stage2-up8-mrg8-results.md`](v015-stage2-up8-mrg8-results.md)：**第二层（`_lt` 专属）当前真值**。`XS_TRIM` 128 家族保留（累计 Fast vs LLPC 收至 +3.56/+4.38/+3.68%）；`LT_UP8` 深层 FP8 判负回退（净 −0.9pp、59.73 dB，含五变体完整取证链与 bias 朝向陷阱）；`LT_MRG8` 按证据跳过；units 越界修复（engine U8-all）。：**当前性能真值**。漏编修复（工具链 `radv_sha256 17cc90d8…`）+ 128 家族 `MLP_SLICE_LOOP [[dont_unroll]]` + Fast/Legacy 双模式 50 核 cmat-address 扩面。Fast 差距 +11.61→**+3.92%** @4K，Legacy ~+10→**+4.10%** @4K；shared 向量化（II'/III'）因 96 家族 +13KB 爆炸判死；相位对齐后 5090 画质全线上调 0.5~2.0 dB。

2. **`history/`（按演进顺序列出的独立历史快照，只增不改）**：
   - [`history/01-330f0b8-init-validation-suite.log`](history/01-330f0b8-init-validation-suite.log)：初始 D3D12 Direct / Split / 127 槽位 / Vulkan 桥接验证套件。
   - [`history/02-c8db22b-direct-release-and-matrix.log`](history/02-c8db22b-direct-release-and-matrix.log)：Direct Release 钩子与状态恢复验证。
   - [`history/03-bf310eb-k2-occ-pad-ab.log`](history/03-bf310eb-k2-occ-pad-ab.log)：1080p K2 `LDS_OCC_PAD` 初探。
   - [`history/04-aef4b89-w1-occ-pad-4k.log`](history/04-aef4b89-w1-occ-pad-4k.log)：补齐 4K K2 `LDS_OCC_PAD` 测试（暴露出 4K 负优化问题，见 `pr-review-d6f70fb.md`）。
   - [`history/05-7c36cf9-openrr-bench-1440p-4k.log`](history/05-7c36cf9-openrr-bench-1440p-4k.log)：引入 `openrr_bench` 1440p / 4K 端到端与显存基准。
   - [`history/06-0dc2625-b0-b1-b2-draft1.log`](history/06-0dc2625-b0-b1-b2-draft1.log)：B0+B1+B2 初版快照（squash 前提交 `0dc2625`，含 `f0+0` 竞争与旧 PSNR 口径，见 `pr-review-0dc2625.md`）。
   - [`history/07-c671f57-b0-b1-b2-draft2.log`](history/07-c671f57-b0-b1-b2-draft2.log)：B1 迁至 `t0` 尾部 + SPIR-V 哈希门禁 + 3 轮 `d6f70fb` 同进程对照（squash 前提交 `c671f57`，见 `pr-review-c671f57.md`）。
   - [`history/08-3dcaf67-b0-b1-b2-draft3.log`](history/08-3dcaf67-b0-b1-b2-draft3.log)：关闭噪声内 `LT_ATTR_RCP/DEPTH`、收紧 PSNR 门禁、加入 B1 `f0` 负对照（squash 前提交 `3dcaf67`，见 `pr-review-3dcaf67.md`）。
   - [`history/09-bdf4188-b0-b1-b2-baseline-final.log`](history/09-bdf4188-b0-b1-b2-baseline-final.log)：B0+B1+B2 基线定版（squash 前提交 `bdf4188`，现已合并为 `90fea26` + `asi/docs` 基线提交；`wide` 显存恢复 `+16 B/px`、顺序 `0dc2625` 负对照、29 缓冲重叠审计、`enc0_attr` SPIR-V 哈希核验）。

3. **`runs/`（`openrr_bench.exe` 原始运行时日志与 `summary.json` 归档）**：
   - [`runs/2026-11-08-b0-b1-b2-baseline/`](runs/2026-11-08-b0-b1-b2-baseline/)：基线定版 1080p / 1440p / 4K 三轮同进程 A/B 与 4K Legacy 的原始 `openrr_baseline.log`、`openrr_candidate.log` 与 `summary.json`。

## 自动归档命令

后续每完成一个阶段（如阶段 2、阶段 3），统一通过以下脚本自动执行测试、写入 `runs/<date>-<tag>-<sha>/` 与 `history/<NN>-<sha>-<tag>.log`，并自动清理 `build/asi/` 下的临时目录：

```bash
python3 asi/build/archive_evidence.py --tag <stage-slug>
```
