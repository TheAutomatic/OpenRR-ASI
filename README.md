# OpenRR-ASI (`openrr-amd.asi`)

简体中文 | [English](README.en.md)

本仓库是基于上游 [OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR) 构建的**独立 OptiScaler `.asi` 插件分支**，用于在 AMD Radeon RX 9000 系列（RDNA4）及实验性支持的 RX 7000 / RDNA3 / RDNA3.5 显卡上运行 NVIDIA DLSS Ray Reconstruction（光线重建，`nvngx_dlssd.dll` 310.9.1）。

关于 OpenRR 的网络重实现原理、RDNA4 着色器架构、Clean-room 验证方法与基准数据，请直接参阅上游主仓库 **[OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**，本文不再赘述。

---

当前发行版本 **v0.1.7plus** 整合上游 **v0.1.7**（含 RDNA4 固定 ACO 路线、RX 7000 / RDNA3 `shaders-gfx11` 路径，以及通用 Windows + Linux Wine / Proton 系统 Mesa RADV >= 26.3 原生支持），并保留本分支的 ASI 集成、菜单和 GPU 资源生命周期保护。v0.1.7 Stage 10 审计与复测证据见 [v017-stage10-audit.md](docs/test-evidence/v017-stage10-audit.md)，旧版本历史数据见下方表格。

## 本仓库做了什么

### 可选 OptiFG 宿主模式

通用发行配置保持 `openrr-amd.ini` 中 `[OpenRR] OptiFGHost=false`，默认走独立 RR 路径。需要 **Upscaler 输入帧生成**（例如 `FGInput=upscaler`、`FGOutput=xefg`）的用户，可按 [OptiFG 安装说明](asi/OPTIFG.md) 配置额外的 `openrr-ngx-host.dll`，再将 `OptiFGHost` 设为 `true` 并重启游戏。不要使用旧测试包的 `_nvngx.dll`。

启用宿主模式后，在菜单关闭 FG 不会自动退出宿主模式；恢复独立 RR 需设回 `false` 并重启。宿主包装和参数处理可能增加开销，FG 本身另有资源准备与插帧成本，尚未量化相对独立 RR 的性能差异。仅替换新版 ASI、保持开关关闭，不会启动这条宿主桥接。

上游 OpenRR 默认编译为外部 `OpenNrRrRuntime.dll`，需要搭配专门修改过的定制版 OptiScaler 才能调用。本仓库将其封装为面向**官方原版 [OptiScaler](https://github.com/optiscaler/OptiScaler)** 的单文件插件 `openrr-amd.asi`，并补充了以下核心特性与修复：

### 1. 单文件 `.asi` 插件与纯 MinGW 构建链
- **零外部运行时 DLL**：将 `OpenNrRrRuntime` 与 `gpu` 引擎直接静态内嵌进单个 `openrr-amd.asi`（Release strip 后约 3.4 MB），放入任何官方原版 OptiScaler 的 `OptiScaler\plugins\`（或旧版 `plugins\`）目录即可直接接管 DLSS Ray Reconstruction（DLSS-D）。
- **极简依赖与跨平台兼容**：提供纯 MinGW-w64 一键构建脚本（`asi/build/build_asi.sh`），静态链接运行时库并通过汇编桩按需延迟解析 Vulkan 入口函数；PE 导入表仅含 5 个系统基础 DLL（`KERNEL32` / `USER32` / `SHELL32` / `msvcrt` / `bcrypt`），同时兼容 Windows 原生与 Linux Wine / Proton（RADV）。

Three modes, picked in the menu or with `Mode` in `[OpenNR-RR]` (`new`, `fast`, `legacy`), which flip live:

| | Performance 1080p | Quality 1080p | Against NVIDIA's own (RTX card) |
|---|---|---|---|
| **New** (default): cheaper kernels | about 2.0 ms | about 2.9 ms | about 60.8 dB on the display |
| **Fast (experimental)**: New with more cut, and the network's deepest levels read from the previous frame | about 1.8 ms | about 2.75 ms | about 55 dB on the display |
| **Legacy**: the kernels closest to NVIDIA's | about 2.2 ms | about 3.27 ms | about 63 dB on the display |

Measured on an RX 9070 XT, outside a game. The first start builds the kernels of New and Fast in the background, with the
image on Legacy until they're ready. Fast changes more from frame to frame in motion and on very bright points: look at
those before keeping it.

### 2. 完整的 NGX 拦截、状态恢复与多后端路由
- **非阻塞能力探测**：自动改写 NGX `SuperSamplingDenoising` 能力参数；首次查询仅做同步轻量检查（校验 `nvngx_dlssd.dll` 310.9.1 版本号 + 实例级 Vulkan RDNA4 特性查询，带 30 秒失败缓存），将权重解析与管线编译全部放到 `CreateFeature` 后台线程，不阻塞游戏主线程。
- **D3D12 Direct 无侵入 Compute 状态恢复**：录制全程不调用 `SetDescriptorHeaps`、不触碰 Graphics Root 状态（描述符通过 Root Constant 裸地址直读）；在发现 D3D12 设备时通过 MinHook 挂钩 `D3D12Core` 底层函数（不触碰对象虚表），只读观测宿主 Heap 切换以剔除失效表句柄，并在 `Record` 后完整恢复游戏的 Compute Root Signature、全部 Compute Root 实参（Descriptor Table / CBV / SRV / UAV / 32-bit Root Constants）以及 PSO / State Object。无需依赖定制版 OptiScaler 内部恢复逻辑，即可原生支持《生化危机 9 (`re9.exe`)》《怪物猎人：荒野》《龙之信条 2》《007 First Light》等游戏。
- **D3D12 ↔ Vulkan 桥接自动回退与硬化**：在无 `amdxc64.dll` ELF 直通的环境（如 Linux vkd3d-proton，或手动指定 `Backend = vulkan`）下自动回退至命令列表切分（Split）+ Vulkan 共享资源互操作桥接；补齐防自递归回退保护以及 Split Barrier / Enhanced Barrier / RTAS / MetaCommand 拒绝规则。
- **原生 Vulkan 游戏直通（`RecordVk`）**：通过 `LdrRegisterDllNotification` 监听晚加载的 `vulkan-1.dll`，在游戏 `vkCreateDevice` 时自动注入所需扩展、特性与独立计算队列，并在设备未就绪时自动隐藏 RR 选项避免崩溃。
- **无全局 DXGI 伪装时的 Streamline RR**：配合 `OptiPatcher.asi`（`Dxgi=auto/false`）时，OpenRR 仅在 `sl.dlss_d.dll` 的 `slOnPluginLoad` 同步调用期间，把 OpenRR 可用的 AMD 适配器在 `sl.common` 的 SystemCaps 中临时显示为 NVIDIA，返回后逐字段恢复，并从插件返回的 JSON 中去掉两个 NVX 专用 Vulkan 扩展。已知限制：假定 slInit 加载插件期间没有其他线程读取这份 caps。

### 3. 与 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 无缝共存
- 当 `OptiScaler\plugins\` 目录下同时存在 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 与 `openrr-amd.asi` 时，无需修改任何一方配置：OpenRR 会自动启用 D3D12 Direct 模式（不叠加第二层命令列表 vtable hook），自动识别并追踪切分后的 continuation 命令列表，并在菜单打开时透传 `Insert`（OptiScaler）与 `End`（`dlssnr-amd`）快捷键。

### 4. 内置 ImGui 停靠侧边栏
- 默认跟随 `OptiScaler.ini [Menu] ShortcutKey`（默认 `Insert`）同步开关，并根据 OptiScaler 的菜单缩放比例自动停靠在 OptiScaler 主面板右侧（支持 SDR、HDR10 PQ 与 scRGB 合成）。
- 支持游戏内实时切换 `Fast` / `Legacy` 网络模式、D3D12 后端、深度类型及各项附加引导图（Guides），修改自动保存至 `openrr-amd.ini`。

### 5. 底层 `gpu/` 与 `runtime/` 修复（已独立提交，便于上游合入）
- **严格 GPU 退休证明与 Overflow 复用池**：移除按录制次数或超时猜测的槽位回收，改为结合 GPU 时间戳写回与游戏侧 `ID3D12CommandAllocator::Reset` / `VkCommandPool` 重置证明；64 个主槽位（自建列表独立占用 `kOwnSlot`）配合 64 个按需复用的 Overflow 槽池，满载时安全跳帧不抛异常。
- **录制期大缓冲 GPU 端填充与批量初始化**：修复 D3D12 绑定状态下 `fill` 超过 64 KB 上传环抛异常、以及 Vulkan 绑定状态下 2 参 `fill` 误重置游戏 `VkCommandBuffer` 的问题，统一改为 GPU 端 `clear_range.comp` 分段派发与 `vkCmdFillBuffer`，并在 `Engine::resize` 期间合并为单次批量提交。
- **双轨 GPU 计时、按需显存分配与残影消除**：同时记录并展示原始 GPU 耗时（`raw` last / p50 / p95）与过滤跨队列异步计算抢占后的估计耗时（`clean`）；Vulkan Host 输入/输出缓冲改为按需紧凑分配（全直通时省去 60~130 MB 显存）；暂停或打开游戏菜单超过 250 ms 返回时自动置位 `ONR_RR_FRAME_RESET` 消除切回瞬间的旧历史残影。

---

## 运行要求

- **显卡与驱动**：
  - **Windows RDNA4（RX 9000 系列，`gfx1201`/`gfx1200`）**：固定严格走离线预编译 **ACO** 路径；Vulkan 需支持 `VK_KHR_cooperative_matrix`、`VK_EXT_shader_float8`（`shaderFloat8` + `shaderFloat8CooperativeMatrix`）、`VK_KHR_workgroup_memory_explicit_layout`、`VK_KHR_pipeline_binary` 与 `maintenance5`，Windows D3D12 Direct 模式需 `amdxc64.dll` 提供 `IAmdExtD3DDevice5`。
  - **Linux Wine / Proton（Mesa RADV >= 26.3）**：直接调用系统 Mesa RADV（无需自定义 `libvulkan_radeon.so` 或启动包装脚本）；RDNA4 原生运行 ACO 并对 `shaders/` 执行内存级 `SpvDecorationNoContraction` 硬化，通过 `winevulkan.dll` 与 `vkd3d-proton` `RecordVk` 互操作运行。
  - **RDNA3 / RDNA3.5（RX 7000 系列 / 780M 等，`gfx11`）**：固定使用独立 `shaders-gfx11/` FP16 / LLPC 路径。
- **Vulkan**：Vulkan 1.3，支持 `hostQueryReset` 与 `scalarBlockLayout`。
- **模型权重文件**：`nvngx_dlssd.dll` 版本必须为 **310.9.1**。

---

## 安装与使用

### 方式一：使用一键安装器（推荐）

推荐使用 **[dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)** 启动器进行自动部署、配置管理以及与 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 的共存安装。

### 方式二：手动配合官方原版 OptiScaler（Windows / Linux 通用发行包）

将构建产物（共 `392` 个文件的通用 `stage/` 包，或直接解压 `dist/OpenRR-v0.1.7plus-OptiScaler.zip`）放入游戏目录（即 OptiScaler 的 `dxgi.dll` / `OptiScaler.ini` 所在目录）：

| 文件 / 目录 | 放置位置（相对于 `OptiScaler.ini` 所在目录） |
|---|---|
| `openrr-amd.asi` | `OptiScaler\plugins\openrr-amd.asi`（旧版 0.7/0.8 无 `OptiScaler\` 子目录时放入 `plugins\openrr-amd.asi`） |
| `openrr-amd.ini` | `openrr-amd.ini` |
| `openrr-amd\shaders\*.spv` | `openrr-amd\shaders\*.spv`（共 `94` 个 `.spv`） |
| `openrr-amd\shaders-gfx11\*.spv` | `openrr-amd\shaders-gfx11\*.spv`（共 `94` 个 `.spv`，RDNA3 / RDNA3.5 必需） |
| `openrr-amd\aco\gfx1201\openrr-d3d12-v1\`、`openrr-amd\aco\gfx1200\openrr-d3d12-v1\` | 保持相同目录结构，每目标含 `94` 份 `.nrp`、`manifest.json` 和 `manifest.msgpack`；Windows RDNA4 严格 ACO 必需，Linux Mesa RADV 自动忽略 |
| `openrr-amd\lang\`、`openrr-amd\OpenRR_*.{txt,md}` | `openrr-amd\lang\`（多语言与 `fonts\`），以及包内随附的 `OpenRR_LICENSE.txt`、`OpenRR_THIRD_PARTY.md` |
| `nvngx_dlssd.dll`（**310.9.1**） | 游戏目录或 `openrr-amd\nvngx_dlssd.dll` |

在 `OptiScaler.ini` 中开启 ASI 插件加载（OptiScaler 默认关闭该项）：

```ini
[Plugins]
LoadAsiPlugins=true
```

#### Linux（Wine / Proton + 系统 Mesa RADV >= 26.3）补充说明

- 使用同一份通用发行包即可直接在 Linux Wine / Proton 下运行：**只需系统安装 Mesa RADV >= 26.3**（无需打包或注入自定义 `libvulkan_radeon.so`，也无需 `with_mesa263.sh` / `VK_ICD_FILENAMES` 包装脚本）。
- 运行时优先通过 `winevulkan.dll` 解析 Vulkan 入口、使用 `vkd3d-proton` 原生 `RecordVk` 互操作，并在加载阶段对 `shaders/*.spv` 注入 `SpvDecorationNoContraction` 硬化直接交由系统 RADV 编译（自动跳过 Windows PAL 专用的 `aco\*.nrp` 离线记录）。
- 部署要点：游戏目录下除 OptiScaler（`dxgi.dll`、`OptiScaler.ini` 设 `LoadAsiPlugins=true`）与游戏自带或自备的 `nvngx_dlssd.dll`（**310.9.1**）外，仍需将 **`OptiScaler/plugins/openrr-amd.asi`**（注意 Linux 下 `OptiScaler` 必须保持首字母和 `S` 大写，才能与 OptiScaler v10 官方 `OptiScaler/plugins/` 目录合并）、`openrr-amd/`（`shaders/`、`shaders-gfx11/`、`lang/`）与 `openrr-amd.ini` 完整复制到游戏目录（不能只放单个 `.asi`；直接解压发布压缩包即可），并在 Steam 启动选项设置 `WINEDLLOVERRIDES="dxgi=n,b" %command%`。

> **重要提示**：
> 1. **切勿将插件改名为 `*-loadlate.asi`**：OptiScaler 会将 `loadlate` 插件放到独立线程并延迟 `LateAsiPluginsDelay`（默认 30 秒）再加载。届时游戏早已完成 NGX 能力查询与 `vkCreateDevice`，OpenRR 无法改写光线重建能力或准备 Vulkan 设备。
> 2. **与 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 共存**：将 `dlssnr-amd.asi` 与 `openrr-amd.asi` 直接同放在 `OptiScaler\plugins\`（或旧版 `plugins\`）下即可（均不要加 `loadlate`）。OptiScaler 按文件名字母顺序加载，`dlssnr-amd` 在 `openrr-amd` 之前，因此 OpenRR 后装 hook、处于 NGX 钩子链最外层，优先接管光线重建调用，无需改动 `dlssnr-amd` 的任何配置。

---

## 编译构建与打包（本地 / GitHub Actions）

需安装 `x86_64-w64-mingw32-g++`、`python3` 与 `glslangValidator`：

```bash
bash asi/build/build_asi.sh
```

构建完成后，打包目录位于 `build/asi/stage/`（分离出的调试符号保存在 `build/asi/openrr-amd.debug`，单元测试与命令列表切分验证套件位于 `build/asi/test_asi_units.exe`）；当存在双目标离线 ACO 清单或设置 `OPENRR_PACKAGE_ZIP=1` 时，会自动校验包结构、大小写与文件哈希并在 `dist/` 生成通用发行包 `dist/OpenRR-v<version>-OptiScaler.zip` 及 `.sha256` 校验文件（Windows 下也可直接运行 `python asi/build/package_release.py` 或 `asi/build/PACKAGE_LOCAL_RELEASE.cmd` 单独打包）。默认离线构建固定版本 Mesa 的 global/vector8 ACO 记录需要现有 WSL Mesa 源码与依赖，详见 [ACO 构建说明](asi/build/mesa/README.md)；云端 CI 通过 [`.github/workflows/package.yml`](.github/workflows/package.yml) 在 Ubuntu 26.04 容器中自动拉取 Mesa 26.3.0 / `libdrm` / Noto CJK 字体、完成完整构建与单测校验并上传或发布 Release 压缩包。stage 同时在 `openrr-amd/` 下包含 `OpenRR_LICENSE.txt` 和 `OpenRR_THIRD_PARTY.md`，不包含 OptiScaler 或外部权重 DLL。

---

## 测试与验证矩阵

硬件环境：**AMD Radeon RX 9070 XT（RDNA4，gfx1201）**，驱动 **26.10.1 (LLPC)**，`nvngx_dlssd.dll` **310.9.1.0**。完整实机测试与基准证据见 [`docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log)。

| 验证项 / 场景 | 路径 / 后端 | 实测结果 | 证据日志 |
|---|---|---|---|
| 命令列表复制虚表防递归与 `CreateCommandList` 回退（#11） | D3D12 Split / Hook | **PASS**（零自递归、零 `E_FAIL`） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| Split Barrier / Enhanced Barrier / RTAS / MetaCommand / Bundle 拒绝（#12） | D3D12 Split `prepare()` | **PASS**（5 类不可切分状态全部拦截，`END_ONLY` 恢复） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 快捷键解析（单数字 `'1'` / 十进制 `'45'` / `'0x2D'` / 组合键）（#13） | `openrr::pe::parse_key` | **PASS** | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 256 KB（`> 64 KB` 上传环）绑定与非绑定 `D3dDevice::fill`（#2） | D3D12 Direct `clear_range` | **PASS**（`unbound wrong=0, bound wrong=0`） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 127 槽位满载 `ONR_RR_BUSY` 与 `CommandAllocator::Reset` 回收（#3/#4） | `OnrRrRecord` D3D12 Direct | **PASS**（前 127 槽 `ONR_RR_OK`，第 128 槽返回 `ONR_RR_BUSY (-5)`，Reset 后立即恢复 `ONR_RR_OK`） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| D3D12 ↔ Vulkan 共享资源互操作桥接（#7） | `RecordInputs` / `Enqueue` / `RecordOutput` | **PASS**（`backend=0 (VULKAN)`，三阶段均返回 `ONR_RR_OK (0)`） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 1080p & 4K 稳态、Fast/Legacy A/B、DRS、K2 哈希门禁与 B1/B2 剖析（K1/K2/#9/#10） | D3D12 Direct Engine | **PASS**（同进程 3 轮 A/B 对比 `d6f70fb`：1080p `Fast` `median=3.124 ms` vs `3.171 ms` (`−1.5%`)、`VRAM=251 MB`；1440p `Fast` `median=5.391 ms` vs `5.451 ms` (`−1.1%`)、`VRAM=435 MB`；4K `Fast` `median=11.812 ms` vs `11.941 ms` (`−1.1%`)、`VRAM=960 MB`；`B1` 独立池化 + `wide` 路径 + `f0` 竞争反向对照 `max_ulp_diff=0`；`B2` Fast 有损近似 `PSNR_01=55.5–60.4 dB`、`PSNR_tonemap=59.9–65.2 dB`；`K2` `spv_hash_nopad=EXACT`） | [`rx9070xt-d3d12-validation-2026-11-08.log`](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |

## 许可证

MIT License（详见 [LICENSE](LICENSE) 与 [THIRD_PARTY.md](THIRD_PARTY.md)）。
