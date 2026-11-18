# Mesa 26.2.4 与 26.3.0 对照测试交接

在 RX 9070（gfx1201、wave32）上，对比两版 Mesa 离线生成的 ACO 记录对 OpenRR Windows D3D12/PAL 的影响。固定 shader、权重、runtime、驱动和测试输入。交付画面、稳态与动态分辨率（DRS）耗时、显存、机器码和资源占用的实测对照，供升级决策。

## 当前状态

- 工作树为 `C:\Users\Kenny\.codex\worktrees\mesa-263-comparison\OpenRR`，分支 `codex/mesa-263-comparison`，基线源码提交 `934efd9`。原 ACO 工作树为 `C:\Users\Kenny\.codex\worktrees\mesa-aco-0101\OpenRR`。
- `build/mesa-comparison/baseline-data` 已复制现有交付数据，包括 Mesa 26.2.4 的 ACO 记录和 manifest。Mesa 固定提交为 `96cb43121031992b85767f9c1be8f3f48e22b1d2`。`build/mesa-comparison/baseline-toolchain` 保留原捕获工具及配置，`build/aco-runtime-trace` 已复制 `openrr_bench.exe` 和 `openrr-trace.asi`。
- 26.3.0 源码包位于 `C:\Users\Kenny\Downloads\OpenRR\build\mesa-version-review\mesa-26.3.0.tar.xz`，已解压至 WSL 的 `~/work/openrr-mesa-263-comparison/mesa-26.3.0`。保留 tarball，接手后记录 SHA256。[官方源码包](https://archive.mesa3d.org/mesa-26.3.0.tar.xz)
- 两处未提交改动为 `asi/build/mesa/prepare_capture.py` 及其生成的 `capture.patch`。该脚本已在 26.3.0 源码上运行，跳过由上游实现覆盖的分歧 push constant 补丁，其余补丁锚点通过。尚未应用 `prepare_global_address.py`，尚未构建、捕获或运行 GPU 测试。从干净解压目录可重做，直接在已打补丁的目录重跑捕获补丁脚本会触发防重复检查。

## 1. 锁定样本并编译

1. 对基线 manifest、NRP、SPIR-V、测试 EXE、ASI 及使用的权重做 SHA256 清单，记录 Mesa 提交、编译选项和指纹。保留现有基线产物。若重跑 26.2.4 捕获以获取诊断，另建输出目录，并核对重建记录与基线的机器码和资源元数据。
2. 为 26.3.0 使用独立源码、Meson/Ninja 构建和捕获工具输出目录，复用 `asi/build/mesa/build_capture.sh` 的构建参数与依赖配置。现有脚本硬编码 26.2.4 的 Git 提交，并从该提交恢复源码文件。给 26.3.0 提供单独的实验入口和工具链配置，记录源码包身份及实际补丁哈希。
3. 复用补丁生成器及 `aco_capture.cpp`。保持双 sentinel、`global` 精度、`vector8` 矩阵加载，以及仅对 `dec_dfn_96_128_fl_lt`、`enc_stage_128_128_160_fl_lt` 启用的 bounded cmat address。两边编译相同 SPIR-V。记录适配过程中必需的精度、寻址或 PAL 行为变更，必要时补单因素对照。
4. 检查 26.3.0 描述符布局、shader 配置和 PAL 导入映射，尤其是 SGPR、push/spill、scratch 和 `MEM_ORDERED` 是否符合 gfx1201 实际规则。复用现有 ABI 探针，验证分歧 push constant 和描述符数组索引。完整捕获继续执行双 sentinel 一致性及 m0 写入、常量数据、PC 相对代码检查。
5. 复用 `asi/build/build_aco_records.py`，传入独立的 `--toolchain`、`--capture`、`--capture-dir` 和 `--out`。对照基线 manifest 覆盖全部输入哈希。若交付 shader 目录覆盖不足，参考 `asi/build/build_asi.sh` 在原构建目录定位其他输入。用 `asi/tests/verify_aco_delivery.py` 或其现有校验逻辑核对记录哈希及 manifest。候选数据放在 `build/mesa-comparison/mesa-263/data`，使用相同数据快照并换入新版记录。

## 2. GPU 对照

1. 先跑 1080p（1280×720 → 1920×1080）和 4K（2560×1440 → 3840×2160）正确性。复用 `asi/tests/run_aco_screen.ps1` 的 `Correctness` 跑 Fast。Legacy 直接调用同一 benchmark 的 `--fast-mode 0`，或给现有脚本加模式参数。两边都用 `aco-strict` 和各自 data 目录，核对运行日志实际加载的记录、哈希和 runtime 路径。
2. 记录逐帧 PSNR、tone-mapped PSNR、P99/最大误差、非有限值和输出哈希。沿用现有 Fast 门槛和 Legacy 逐位一致性要求。对差异最大的帧保留两版画面与同尺度差分图，记录阶段、帧号和区域。逐位一致的输出用哈希证明。若失败，使用现有 trace 和 frame-state 找首次分歧。
3. 通过正确性后跑 `Long`。沿用 6 对交替顺序，每轮 48 warmup、240 steady、64 DRS。关闭 trace、stamps 和逐帧读回。报告每对及汇总的 GPU 时间中位数、P95 和 DRS 尾部，记录驱动、模式及实际执行路径。定位耗时差异时再跑 `Profile`，单独保留结果。已有固定场景与游戏回放工具可用时，再做同驱动、同画质的短回放复核，并标明与合成 benchmark 的覆盖范围。
4. 分别记录 runtime 自报的 feature VRAM 和 DXGI 本地显存进程增量，取稳态及 DRS 样本。现有 `summary.json` 仅在 candidate 段输出稳态显存。可用交替顺序覆盖两版，按 `order.json` 映射实际角色。若现有控制台和日志缺少 DRS 样本，给原 benchmark 增加最小的对称 JSON 输出，用同一新编译的 EXE 重测两侧。可获取峰值时标明采样频率和测量口径。

从对比工作树根目录执行 Fast 正确性测试：

```powershell
& asi/tests/run_aco_screen.ps1 -Name mesa-263-1080p -Baseline build/mesa-comparison/baseline-data -Candidate build/mesa-comparison/mesa-263 -PerDataRecords -Stage Correctness
```

4K 增加 `-RenderWidth 2560 -RenderHeight 1440 -OutputWidth 3840 -OutputHeight 2160`，并改用独立名称。正确性通过后可用同一名称切换到 `-Stage Long`。重跑同一阶段时使用新名称。

## 3. 交付

- 用 `asi/build/mesa/compare_review_records.py` 或同类脚本，按 SPIR-V 哈希对齐，汇总代码体积、VGPR/SGPR、scratch、LDS、workgroup 和 ABI 元数据。检查热点内核是否有寻址、访存合并或 spill 变化，结合 GPU 耗时解释。
- 复用 `asi/tests/summarize_aco_screen.py` 的轮次映射和汇总方式，补齐 P95、显存和 Legacy 结果。交付对照表、逐轮数据、测试条件、构建及运行日志、画面证据和复现命令。逐帧大读回留在 build 下，归档前记录哈希。
- 结合画面门槛、耗时差异与轮间波动、显存变化及适配成本给出结论。若候选未过 ABI 或画面门槛，交付首个可复现的失败内核或帧及实际错误。
