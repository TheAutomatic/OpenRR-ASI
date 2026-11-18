# OpenRR Stage 10 — 终包复审修复与全链路实机审计（`codex/v017-merge`）

- **工作树 / 分支**：`C:/Users/Kenny/.codex/worktrees/v017-merge/OpenRR`，分支 `codex/v017-merge`（Round 1 提交 `51dbc63` 基于 `b472f91`，Round 2 收口 4 项预发布审查项并整合通用 Windows + Linux Wine/Proton 系统 Mesa RADV 支持；对照工作树 `C:/Users/Kenny/.codex/worktrees/mesa-263-comparison/OpenRR` `2baa3d3` 保持只读未改动——其 `git status --short` 仅存在 Round 2 开始前 `2026/11/18 01:13–01:15` 遗留的本地 `README.md` / `README.en.md` 未提交修改；严格未执行 `git push`）。
- **验证硬件 / 驱动**：
  - 独立显卡：`AMD Radeon RX 9070 XT`（`1002:7550`，RDNA4 `gfx1201` / `mach=0x4e`）
  - 核显 / RDNA3 验证卡：`AMD Radeon 780M Graphics`（`1002:1900`，RDNA3 `mach=0x44`）
  - 驱动版本：AMD Adrenalin `26.11.1 (LLPC)`，测试权重 `nvngx_dlssd.dll 310.9.1.0`。
- **洁净交付包（`build/v017-asi/stage/` 及 `dist/OpenRR-v0.1.7plus-OptiScaler.zip`）**：共 **`392` 个文件、`0` 个 `*.log` 残留**，目录采用 OptiScaler v10 标准 `OptiScaler/plugins/openrr-amd.asi` 并把许可证与第三方声明收入 `openrr-amd/OpenRR_LICENSE.txt` 及 `openrr-amd/OpenRR_THIRD_PARTY.md`（避免覆盖游戏或 Mod 同名文件）；确定性发布压缩包 `dist/OpenRR-v0.1.7plus-OptiScaler.zip`（`23,785,221` B，内嵌提交戳 `0.1.7plus (8455873)`，SHA-256 `0f1babdf307039a1e00f87b9aeb82d3fb3c69cdcfebcbe54a0c5c2195d7b7bf6`，WSL 与 Windows 原生 Python 打包逐比特一致；Round 1 证据留存于 `build/v017-asi/final-audit-evidence/`，Round 2 运行时/GPU 矩阵证据留存于 `build/v017-asi/final-audit-evidence-r2/`）。

---

## 1. Stage 10 终包复审核心修复总结

### 1.1 固定发行版 ACO 策略与明确失败路径（Round 1）
1. **RDNA4 一律走 ACO，取消发行版编译器选择与 LLPC 回退**：
   - `runtime/src/runtime.cpp`（`resolve_compiler` / `Create` / `create_vk_host` / `d3d_setup` / `build_network` / `build_lite` / `load_wide` / `load_flow`）：
     - 移除 `ONR_RR_COMPILER` 环境变量读取；`Create()` 保留 `OnrRrCreateInfo::compiler` 字段以维持 ABI 二进制布局不变，但在结构长度覆盖该字段时拒绝任何 `compiler != 0` 覆写。
     - `gfx1201`/`gfx1200`（以及任一 `is_rdna4` 设备）强制启用 `aco = true, aco_strict = true`；RDNA4 设备若未能映射到已交付离线目标（`!aco_target`）或 Vulkan `VK_KHR_pipeline_binary + maintenance5` / 清单校验未就绪，直接硬失败（禁止回退驱动 LLPC）。`gfx11`（RDNA3／3.5）固定使用独立 `shaders-gfx11` FP16／LLPC 路径。
     - 移除 D3D12、Native Vulkan Host 与 D3D12↔Vulkan Bridge 的所有单核、utility、direct/sampler、base、wide、flow 及 `_lt`/`_lx` LLPC 回退分支；移除 D3D12 运行期因 `_lt`/network 失败静默切换 Vulkan 的回退。
   - **故障注入测试钩子隔离到测试构建**：`ONR_ACO_TARGET`、`ONR_ACO_DIR`、`ONR_ACO_DUMP`（`d3d12.cpp`、`rr_pipeline_binary.hpp`、`runtime.cpp`、`aco_runtime_trace.hpp`）及 `ONR_TEST_SPOOF_LOADER`（`vk.cpp`）等仅用于 `test_asi_units.exe`（`#ifdef OPENRR_TEST_HOOKS`），并去除 `ONR_ACO_OPTIONAL`。验证工具（`runtime/validate/main.cpp`、`runtime/validate/vk_main.cpp`）通过临时复制 `ci.data_dir` 与隔离 `ci.cache_dir` 经公开 ABI 对正式版 `openrr-amd.asi` 做故障注入。
   - **二进制字符串扫描实证**：`build/v017-asi/stage/OptiScaler/plugins/openrr-amd.asi`（`v0.1.7plus (8455873)`，`1b49d32b...`；以及此前 `7ed8b47` 构建 `769441a2...`）经 `package_release.py` 校验，`ONR_RR_COMPILER`、`ONR_ACO_OPTIONAL`、`ONR_ACO_TARGET`、`ONR_ACO_DIR`、`ONR_ACO_DUMP`、`ONR_TEST_SPOOF_LOADER`、`ONR_TEST_SIMULATE_RADV` 出现次数均为 **`0`**。
2. **结构化错误定位与异步 `ONR_RR_STATE_FAILED` 传播**：
   - `format_pipeline_error` 统一输出 `[backend=<d3d12|vulkan-host|vulkan-bridge> arch=<...> mach=0x<...> target=<...> pkg=<...> stage=<...> kernel=<...>]: <underlying>`。
   - `build_lite()` 在 `_lt`/`_lx` 加载失败时于锁下卸载部分核、清空 `engine.set_lite(nullptr)`、标记 `lite_failed = true`、`state = ONR_RR_STATE_FAILED`；`Record`、`RecordVk`、`RecordInputs`、`RecordPost`、`PrepareDenoised`、`EnqueueNetwork`、`EnqueuePost`、`RecordOutput` 在检测到 `ONR_RR_STATE_FAILED` 时立即返回 `ONR_RR_FAILED`，且 `asi/src/rr_asi_main.cpp` 在帧录制失败时同步更新菜单／状态机；`OnrRrGetFastMode` / `OnrRrGetFp8` 报告 `3`。
3. **纠正 `OnrRrSetFp8` / `OnrRrGetFp8` 文档口径**：
   - 实际由 `OnrRrSetFp8(ctx, 1)` 返回 `ONR_RR_OK` (`0`) 登记请求，再由 `OnrRrGetFp8(ctx)` 在 `gfx11` 收到开启请求时报告 `3`（实机 `RDNA3 780M OnrRrSetFp8/OnrRrGetFp8 (s0=0 s1=3 s2=0)` 已复验）。

### 1.2 Vulkan `pNext` 零宿主内存写入与结构冲突校验（Round 1）
- `runtime/src/vk_host.cpp` 删除 `RestoreBool` 与 `put_back()` 事后恢复机制；当遇到未知 `sType` 中断深拷贝后，若尾部未克隆结构体存在待修改字段，禁止任何写操作并返回 `ONR_RR_UNSUPPORTED`；RDNA4 `prepare()` 要求设备具备 `VK_KHR_pipeline_binary + maintenance5`（RADV 路径则要求 `VK_EXT_shader_float8` + `cooperativeMatrix`）。
- 预检成环链与互斥特征结构组合（`pEnabledFeatures` + `Features2`、`Vulkan14Features` + `Maintenance5FeaturesKHR` / 拆分 1.1 / 1.2 / 1.3 结构），均在不触碰宿主内存的前提下拒绝。

### 1.3 Vulkan 重建（`CHURN`）与 `vk_harvest` 执行证明（Round 1）
- 修复 `runtime/validate/vk_main.cpp` 副视图模式判定，按显式 `expect_second_network` 判断退出条件，双队列双网络并发重建（`ONR_VALIDATE_VK_SECOND_OUT=960x540`, `CHURN=1`）持续稳定通过且 `0` 设备丢失。
- `gpu/include/opennr_rr/vk.hpp` + `gpu/src/vk.cpp` + `runtime/src/runtime.cpp` 结合 `vhq_seq` GPU 执行序号缓冲与 `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`、`pool_retired()` 彻底隔离未提交丢弃命令缓冲区对计时采样的污染，同时保留独立的 `Grave` 回收条件。

### 1.4 Round 2 预发布审查 4 项修复（`[P1-1]`、`[P1-2]`、`[P2-1]`、`[P2-2]`）
1. **`[P1-1]` 构建声明必需 SPIR-V 集合强校验**（`gpu/include/opennr_rr/kernels.hpp`、`gpu/src/kernels.cpp`、`runtime/src/runtime.cpp`、`asi/tests/test_aco_build_policy.py`、`runtime/validate/{main.cpp,vk_main.cpp}`）：
   - `Kernels::load_lite` / `load_required_variants` 对照 `declared_*_stems()`（`40` 个 `_lt` + `4` 个 `_lx`）精确校验每个构建声明核；缺失任意必需 `.spv`（如 `enc0_stage_32_64_f_lx.spv`、`post_f_lt.spv`）不再默默跳过或把 `_lx` 降级为 3 支 + `_lt` 补位。
   - 错误消息精确输出 `missing required SPIR-V: <fname>.spv` 并收紧 `256` 字节 `OnrRrInfo::message` 前缀以免截断文件名；触发 `OnrRrGetFastMode == 3`，RDNA4 严格 ACO 下同时置 `ONR_RR_STATE_FAILED`。
2. **`[P1-2]` `PrepareVkDevice` 支持规范允许的可重复 `sType` 与指针判环**（`runtime/src/vk_host.cpp`）：
   - 将环检测从按 `sType` 改为按节点指针地址（`const void*`）去重，允许规范（`VUID-VkDeviceCreateInfo-sType-unique`）明确允许重复的 `VkDevicePrivateDataCreateInfo` 与 `VkDeviceDeviceMemoryReportCreateInfoEXT`，同时继续拒绝必须唯一的重复 `sType` 与 `PAGE_READONLY` 零宿主写入保护。
3. **`[P2-1]` ASI Vulkan 重置 hook 仅在驱动返回 `VK_SUCCESS` 后通知**（`asi/src/rr_pe_vkreset.hpp`、`asi/src/rr_asi_main.cpp`）：
   - 为 `vkBeginCommandBuffer`、`vkResetCommandBuffer` 与 `vkResetCommandPool` 全部 12 处导出及 `vkGetDeviceProcAddr` 钩子引入统一辅助器，先调用驱动原始实现并仅在 `r == VK_SUCCESS` 时调用 `OnrRrNotifyVk*` 更新 epoch。
4. **`[P2-2]` `openrr_bench` 按被测版本透传编译器策略与路由核验**（`runtime/bench/openrr_bench.cpp`）：
   - 向基线与候选 `Create()` 原样传入 `aco`（`2`）/ `aco-strict`（`3`）/ `llpc`（`1`），仅在收到 `"release compiler override is disabled"` 且请求为 `aco` / `aco-strict` 时回退 `0`（对明确测试 `llpc` 绝不静默改 `0`），并通过 `VerifyAndParseCompilerRoute` 核验日志实际路线后写入 `summary.json`。

### 1.5 通用 Windows + Linux Wine / Proton（系统 Mesa RADV >= 26.3）整合
- `gpu/src/rr_radv_spirv.hpp`、`gpu/include/opennr_rr/vk.hpp`、`gpu/src/vk.cpp`、`asi/build/vulkan_lazy.sh`、`asi/src/rr_pe_vkd3d_interop.hpp`、`asi/src/rr_asi_main.cpp`：
  - 将通用包统一为单份 392 文件交付（Windows 原生 + Linux Wine/Proton 共享）：在 Linux 系统安装 Mesa RADV >= 26.3 时，无需任何自定义 `libvulkan_radeon.so` 或 `with_mesa263.sh` / `VK_ICD_FILENAMES` 包装脚本。
  - RADV 路径自动跳过 Windows PAL `.nrp` 离线包，通过 `winevulkan.dll` 优先解析与 `vkd3d-proton` 原生 `RecordVk` 互操作，在内存对 `shaders/*.spv` 注入 `SpvDecorationNoContraction` (`42`) 后交由系统 RADV ACO 编译（`94/94` SPIR-V 已通过 `spirv-val --scalar-block-layout` 校验，其中核心 3 支在 WSL Mesa `26.3.0` `RADV GFX1201` 上实编验证）。Windows 严格 PAL `.nrp`（`gfx1201`/`gfx1200`）与 RDNA3 `shaders-gfx11` 路径保持不变。

---

## 2. 终版交付包清单与 SHA-256（`build/v017-asi/stage/` 与 `dist/`）

| 制品 / 目录 | 数量 / 字节数 | SHA-256 / 校验说明 |
|---|---:|---|
| 交付包总计 | `392` 文件，`0` `*.log` | `94` `.spv` (`shaders/`) + `94` `.spv` (`shaders-gfx11/`) + `188` `.nrp` (`aco/{gfx1201,gfx1200}`) + `4` manifests + 插件、宿主桥、`openrr-amd/OpenRR_{LICENSE.txt,THIRD_PARTY.md}` 与多语言字库 |
| `OptiScaler/plugins/openrr-amd.asi` | `3,546,050` B | `1b49d32b1909695acc47447edfb0e8651541abd9fd22cb7ede256b61eb660ba8`（`v0.1.7plus (8455873)`，7 项测试/覆写环境变量字符串扫描全为 `0`；C++/GPU 实机矩阵系基于此前 `7ed8b47` 构建 `769441a2...` 运行） |
| `openrr-ngx-host.dll` | `585,819` B | `714a43c696dd9d3fad44f81e36c88f4068af5bccca1cfa0dd16f63acb50e9296` |
| `openrr-amd.ini` | `2,291` B | `96b17b9c06c5f8c258f34870ca538d20d8ab6556ae530f57fd117174e967f4f9` |
| `aco/gfx1201/openrr-d3d12-v1/manifest.json` | `137,573` B | `0b8f4633c12705d4fdac0854b385faef67ce2dc2bdd85e5058b9eae54c78c001` |
| `aco/gfx1201/openrr-d3d12-v1/manifest.msgpack` | `19,051` B | `87bd4e03028643c8220dc6540853bdb01eabd5cb28ebb66dec7f6babd82818ca` |
| `aco/gfx1200/openrr-d3d12-v1/manifest.json` | `137,573` B | `2b4e8f497ba4527a100388c773e221132d488b5b25e1e5f179471ab808738e2c` |
| `aco/gfx1200/openrr-d3d12-v1/manifest.msgpack` | `19,051` B | `0d9e693c322a0f4252837abb731cf86442d3cbfdf6114c209970f6d0a693d26f` |
| `dist/OpenRR-v0.1.7plus-OptiScaler.zip` | `23,785,221` B | `0f1babdf307039a1e00f87b9aeb82d3fb3c69cdcfebcbe54a0c5c2195d7b7bf6`（`392` 条 POSIX 路径条目，WSL 与 Windows 原生打包逐比特一致，附 `.sha256`） |

> 注：全量 ACO `94/94` `spv_hash`、`code_hash` 及 `188` 份 `.nrp` `record_sha256` 与 Round 1 逐字节一致；仅 `manifest.{json,msgpack}` 顶层 `fingerprint` 因 `aco_capture` 随 `gpu/src/vk.cpp` 更新重链接而变更。
>
> 注：上表中的 `8455873` / `0f1babdf...` SHA-256 记录对应合并压栈前在 `codex/v017-merge` 上的本地预检构建；正式合入 `asi-plugin` 分支后的官方发布压缩包（`OpenRR-v0.1.7plus-OptiScaler.zip` 及 `.sha256`）由 GitHub Actions（`.github/workflows/package.yml`）在纯净环境中可复现构建，`OPENRR_BUILD_STAMP` 对应 `asi-plugin` 提交短哈希。

---

## 3. Round 2 单元测试与回归套件（全量通过）

| 套件 | 命令 / 范围 | 实测结果（证据：`build/v017-asi/final-audit-evidence-r2/` 等） |
|---|---|---|
| Python 单测 | `python -m unittest discover -s asi/tests -p "test_*.py" -v` | **`44/44 OK`**（`7.318 s`，含 6 项 `test_package_release.py` 发布目录/大小写/审计与确定性打包校验） |
| Present Scope 单测 | `test_present_scope.exe` | **`PASS`**（`EXIT=0`，`test_present_scope.txt`） |
| DXGI Queue 单测 | `test_dxgi_queue.exe` | **`PASS`**（`EXIT=0`，`test_dxgi_queue.txt`） |
| C++ GPU/Runtime 单测 | `test_asi_units.exe`（`-DOPENRR_TEST_HOOKS=1`） | **`56/56 [PASS]`、`ALL ASI UNIT TESTS PASSED`**（`EXIT=0`，`test_asi_units.txt`），含 Round 1 51 项及 Round 2 新增 5 项： |
| ↳ RADV `NoContraction` 硬化 | `RADV SPIR-V NoContraction hardener` | `[PASS]`：global/native 拆分注入精确与幂等校验 |
| ↳ `[P1-2]` `PrepareVkDevice` 可重复 `sType` | `VkDevicePrivateDataCreateInfo` / `MemoryReport` 串联 + 成环 / 唯一 `sType` 拒绝（`PAGE_READONLY`） | `[PASS]`：合法多节点放行，成环/非法重复拒绝且宿主零修改 |
| ↳ `[P2-1]` Vulkan 重置 hook 后置通知 | 经实际 detour 调用 `vkBeginCommandBuffer` / `vkResetCommandBuffer` / `vkResetCommandPool` 失败与成功路径 | `[PASS]`：`VK_ERROR_*` 失败时零 epoch 递增，仅 `VK_SUCCESS` 后通知 |
| ↳ `[RADV]` 模拟 Mesa RADV 构建与设备准备 | 模拟 Mesa RADV 特征链 `PrepareVkDevice` + RADV 原生 ACO `Create/Build` | `[PASS]` |
| ↳ `[P1-1]` 缺必需 `.spv` 严格拒绝 | 隔离包移除 `enc0_stage_32_64_f_lx.spv` (`Fast=2`) 与 `post_f_lt.spv` (`Fast=1`) | `[PASS]`：报出准确 `.spv` 文件名，禁止就绪 |

---

## 4. RX 9070 XT 与 780M Round 2 实机矩阵验证结果（基于 `7ed8b47` 终版源码，`769441a2...`）

### 4.1 `[P2-2]` `openrr_bench.exe` 基线编译器透传与路由核验（4 项全通过）
| 用例 | 命令配置 | 实测结果 | 证据 |
|---|---|---|---|
| `bench_p2_2_d3d12` | Stage 9 (`2baa3d3`) `aco-strict` vs Stage 10 R2 `aco-strict` (`--backend d3d12`, `ONR_BENCH_GATE=off`) | `rc=0`，`baseline_compiler_enum=3 (ACO, base_aco=130, llpc=0)`，`candidate_compiler_enum=0 (ACO, cand_aco=100, llpc=0)`，`parity_passed=true`，Steady/DRS `PSNR_01=999.00 dB`, `exact=100%` | `bench_p2_2_d3d12/summary.json` |
| `bench_p2_2_s9_vk_strict` | Stage 9 Bridge `--backend vulkan --baseline-compiler aco-strict` | `rc=1`，输出 `"OpenRR Create failed: strict ACO requires the D3D12 backend"`（证实 `ci.compiler=3` 被原样透传给 Stage 9，未再静默转 `0 -> LLPC`） | `bench_p2_2_s9_vk_strict.stdout.txt` |
| `bench_p2_2_vk_llpc_vs_aco` | Stage 9 Bridge `llpc` vs Stage 10 R2 Bridge `aco-strict` (`--backend vulkan`, `ONR_BENCH_GATE=off`) | `rc=0`，`baseline_compiler_enum=1 (LLPC)`，`candidate_compiler_enum=0 (ACO, cand_aco=82, llpc=0)`，`parity_passed=true`，Steady `PSNR_01=57.5818 dB` / DRS `56.8758 dB`（`0` 非有限值） | `bench_p2_2_vk_llpc_vs_aco/summary.json` |
| `bench_p2_2_reject_llpc` | Stage 10 R2 `--candidate-compiler llpc` | `rc=1`，输出 `"Create: release compiler override is disabled (pass compiler=0)"` | `bench_p2_2_reject_llpc.stdout.txt` |

### 4.2 RX 9070 XT：3 条 RDNA4 路径正常全包 + `[P1-1]` 缺 `.spv` + 损坏 `.nrp`/缺清单全矩阵
均以独立隔离 `ONR_CACHE_DIR` / `ONR_LOG_PATH` 顺序执行（`1280x720 -> 1920x1080`，`12` 帧，`FAST_AT=2, FAST_LEVEL=2`）：

| 测试类别 | D3D12 Direct（`auto`） | D3D12↔Vulkan Bridge（`vulkan`） | Native Vulkan Host（ABI 5） | 验证结果 |
|---|---|---|---|---|
| **正常完整包** | `rc=0`, `(state 4)`, `PSNR 999.00 dB`, `ACO=199, LLPC=0` | `rc=0`, `(state 4)`, `PSNR 999.00 dB` (`100%`), `ACO=163, LLPC=0` | `rc=0`, `(state 4)`, `PSNR 999.00 dB` (`100%`), `ACO=167, LLPC=0` | 3 路 12/12 帧输出哈希逐比特全同（`585ed1c01f7a0d1e .. da61e05f4f7e5d9f`） |
| **缺 `.spv`：`enc0_stage_32_64_f_lx.spv`**（`FAST_LEVEL=2`） | `rc=1`, `ACO=140, LLPC=0` | `rc=1`, `ACO=112, LLPC=0` | `rc=1`, `ACO=116, LLPC=0` | 全部日志准确定位 `stage=fast_lx` + `missing required SPIR-V: enc0_stage_32_64_f_lx.spv`，无 `(state 4)`/`(state 1)` |
| **缺 `.spv`：`post_f_lt.spv`**（`FAST_LEVEL=2`） | `rc=1`, `ACO=83, LLPC=0` | `rc=1`, `ACO=64, LLPC=0` | `rc=1`, `ACO=68, LLPC=0` | 全部日志准确定位 `stage=fast_lt` + `missing required SPIR-V: post_f_lt.spv`，无 `(state 4)`/`(state 1)` |
| **缺 `.spv`：`post_f_lt.spv`**（`FAST_LEVEL=1`） | `rc=1`, `ACO=83, LLPC=0` | — | — | 同上（`fault_spv_lt_lvl1_d3d12_auto`） |
| **故障 1：清单不可用**（`nomani`） | `rc=1`, `ACO=0, LLPC=0` | `rc=1`, `ACO=0, LLPC=0` | `rc=1`, `ACO=0, LLPC=0` | `stage=utility kernel=ingest` / `stage=resolve_compiler kernel=<manifest>` |
| **故障 2：基础核 `.nrp` 损坏**（`enc_stage_64_64_96_f`） | `rc=1`, `ACO=36, LLPC=0` | `rc=1`, `ACO=26, LLPC=0` | `rc=1`, `ACO=30, LLPC=0` | `stage=base kernel=enc_stage_64_64_96_f` |
| **故障 3a：`_lt` `.nrp` 损坏**（`post_f_lt`） | `rc=1`, `ACO=83, LLPC=0` | `rc=1`, `ACO=64, LLPC=0` | `rc=1`, `ACO=67, LLPC=0` | `stage=fast_lt kernel=post_f_lt`，`(state 3)` |
| **故障 3b：`_lx` `.nrp` 损坏**（`enc0_stage_32_64_f_lx`） | `rc=1`, `ACO=139, LLPC=0` | `rc=1`, `ACO=112, LLPC=0` | `rc=1`, `ACO=116, LLPC=0` | `stage=fast_lx kernel=enc0_stage_32_64_f_lx`，`(state 3)` |

### 4.3 Dual-Queue Vulkan `CHURN` 与 Radeon 780M（`gfx11`）
- **Dual-Queue Vulkan `CHURN`（`vk_churn_3b`，`960x540`, `CHURN=1`, `FAST=1`）**：
  - `rc=0`、`ACO=360, LLPC=0`、`0` 设备丢失；`churn 1: 1129x635 and 544x306 -> 960x540, 205 frames of both, network frames 120 / 120, GPU 3.57 / 2.20 ms`（`second feature: 218 frames recorded beside the first, 120 network frames, state 1`）。
- **Radeon 780M（`1002:1900`, `mach=0x44`, `gfx11`）**：
  - **`radeon780m_vkhost_healthy`**（`FAST=2, FAST_AT=2, FAST_LEVEL=2`）：`rc=0`、`all finite`、`(state 4)`、`83.80 ms`、路由 `shaders-gfx11`、`ACO=0, LLPC=167`。
  - **`radeon780m_vkhost_fault_spv_lx`**（`ONR_TEST_REMOVE_SPV=enc0_stage_32_64_f_lx.spv`, `FAST=2, FAST_AT=0, FAST_LEVEL=2`）：`rc=1`、`(state 3)`、`ERROR: fast mode failed (GetFastMode=3)`、日志精确输出 `missing required SPIR-V: enc0_stage_32_64_f_lx.spv`、`ACO=0, LLPC=115`。

---

## 5. 测试边界说明与未本地覆盖硬件

1. **按用户指令“暂时不用测速和5090画质”跳过项目**：
   - 未在 `7ed8b47` / 打包重链二进制上运行长稳态 ABBA 吞吐测试与 RTX 5090 `openrr_contrast_eval.exe` 画质对比。
2. **本机无物理卡未实机覆盖的目标硬件**：
   - **`RX 9060 / 9060 XT`（`gfx1200`, `mach=0x48`）**：已通过离线 `94/94` `.nrp` 校验、`pal::splice` / `pal::splice_vulkan` 与 `test_asi_units.exe` 故障注入，缺真机实跑。
   - **独立 `RX 7000`（`gfx1100`/`1101`/`1102`）与 `RDNA3.5`（`gfx115x`）**：同走 `shaders-gfx11` LLPC 路径，当前仅以 `780M`（`mach=0x44`）与架构表覆盖。
   - **Linux 实机 Mesa RADV >= 26.3 游戏运行**：核心着色器已在 WSL `spirv-val` + Mesa `26.3.0` 验证并通过 `test_asi_units.exe` 模拟 RADV 测试，真机 Proton 游戏待分发给 Linux 测试者实机验证。
