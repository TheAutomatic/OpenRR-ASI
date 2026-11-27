# OpenRR-ASI (`openrr-amd.asi`)

简体中文 | [English](README.en.md)

> [!IMPORTANT]
> **源码公开 · 完全免费 · 严禁倒卖与抹除署名**：本仓库由 `TheAutomatic` 编写/修改的独立 ASI 插件代码、构建打包工具及编译发行产物基于 **[PolyForm Noncommercial License 1.0.0](LICENSE)** 公开授权，仅限个人非商业使用，严禁有偿售卖、收费整合包/网盘/群分发及收费代安装。**二次修改/衍生分发须严格遵守 [NOTICE](NOTICE)**：允许署二改作者名，但必须完整保留原始作者（`TheAutomatic`）与全部上游作者（[OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR)、[mochizuki0323/DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 等）署名、官方仓库链接及 `Required Notice:` 声明，严禁移除署名或篡改二进制信息/版本号冒充原创。如付费购买请立即申请退款并举报卖家！详见[许可证、免费声明与署名要求](#许可证)。

本仓库是基于上游 [OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR) 构建的**独立 OptiScaler `.asi` 插件分支**，用于在 AMD Radeon RX 9000 系列（RDNA4）及实验性支持的 RX 7000 / RDNA3 / RDNA3.5 显卡上运行 NVIDIA DLSS Ray Reconstruction（光线重建，`nvngx_dlssd.dll` 310.9.1）。

关于 OpenRR 的网络重实现原理、RDNA4 着色器架构、Clean-room 验证方法与基准数据，请直接参阅上游主仓库 **[OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**，本文不再赘述。

---

当前发行版本 **v0.1.7.1plus** 完成了面向 **OptiScaler v0.10 nightly（Plugin API v1–v3）** 的原生插件化重构，在保留 **v0.1.7plus6**（Linux Wine / Proton + `vkd3d-proton` 直通 Vulkan interop 修复）、**v0.1.7plus5**（`NvAPI_GPU_GetGPUInfo` `0xafd1b02c` 通用回退兼容与并行化构建打包链）与 **v0.1.7plus4** / **v0.1.7plus3**（ReShade + RenoDX D3D12 包装层兼容性修复与 D3D12 装钩加固）全部成果的基础上，实现了以下架构升级：
- **原生 `OptiProvider_Dlssd` 提供者注册**：通过 OptiScaler Plugin API v3 `RegisterProvider(..., "OpenRR", OptiProvider_Dlssd, ...)` 直接作为 OptiScaler 官方一级 Ray Reconstruction 提供者接入，彻底移除外层 ~30 个 `NVSDK_NGX_*` MinHook、`SlCapsPatch` 及独立的 `openrr-ngx-host.dll` 宿主桥接，原生兼容 OptiScaler 全部下游特性（`OptiFG`、`XeFG`、`HUDfix`、`OutputScaling`、`RCAS`、DLSS-D / FSR / XeSS 实时切换），无需任何额外配置或重启切宿主模式。
- **原生嵌入 OptiScaler `PLUGINS -> OpenRR` 标签页**：彻底移除自建 DX11/DX12/Vulkan ImGui 叠加层、SwapChain / `Present` hook、WndProc / User32 按键拦截及内置字体，界面直接嵌入 OptiScaler 主菜单左侧 `PLUGINS -> OpenRR` 标签页；内置一键 `Re-init Upscaler` 按钮与零 Hook 中文/日文/韩文字体配置（自动写入 `OptiScaler.ini [Menu] UseHQFont=true` 与 `TTFFontPath`，未配置时安全回退英文避免乱码），剥离后体积大幅缩减且完全消除跨渲染后端叠加层冲突。
- **版本要求**：本版本起仅适配 **OptiScaler v0.10 nightly**（需支持 Plugin API v3 `RegisterProvider`）；不再需要旧版 `openrr-ngx-host.dll`、`MenuKey`、`OptiFGHost` 等旧配置项，升级前建议清理旧版 `openrr-ngx-host.dll` 并使用新版 `openrr-amd.ini`。

同时继续完整整合上游 **v0.1.7**（含 RDNA4 固定 ACO 路线、RX 7000 / RDNA3 `shaders-gfx11` 路径，以及通用 Windows + Linux Wine / Proton 系统 Mesa RADV >= 26.3 原生支持），并保留本分支的 GPU 资源生命周期保护与性能/显存优化。v0.1.7plus 完整速度、画质与显存实测对比见 **[v017plus-speed-quality-eval.md](docs/test-evidence/v017plus-speed-quality-eval.md)**（[English](docs/test-evidence/v017plus-speed-quality-eval.en.md)），Stage 10 审计与复测证据见 [v017-stage10-audit.md](docs/test-evidence/v017-stage10-audit.md)，旧版本历史验证记录保留于文末表格。

## 本仓库做了什么

上游 OpenRR 默认编译为外部 `OpenNrRrRuntime.dll`，需要搭配专门修改过的定制版 OptiScaler 才能调用。本仓库将其封装为面向**官方原版 [OptiScaler](https://github.com/optiscaler/OptiScaler) v0.10 nightly** 的单文件原生插件 `openrr-amd.asi`，并补充了以下核心特性与修复：

### 1. 单文件原生 `.asi` 插件与纯 MinGW 构建链
- **零外部运行时 DLL**：将 `OpenNrRrRuntime` 与 `gpu` 引擎直接静态内嵌进单个 `openrr-amd.asi`（移除独立 ImGui 与 DX11/DX12/Vulkan 渲染叠加层后，体积更轻、依赖更少），放入官方 OptiScaler v0.10 nightly 的 `OptiScaler\plugins\` 目录即可通过 `OptiPluginInit` 自动注册为 `OpenRR` 光线重建提供者与设置标签页。
- **极简依赖与跨平台兼容**：提供纯 MinGW-w64 一键构建脚本（`asi/build/build_asi.sh`），静态链接运行时库并通过汇编桩按需延迟解析 Vulkan 入口函数；同时兼容 Windows 原生与 Linux Wine / Proton（RADV）。

#### 三档网络模式与实测表现（RX 9070 XT，`v0.1.7plus`）

可在 OptiScaler 主菜单 `PLUGINS -> OpenRR` 标签页或 `openrr-amd.ini` 的 `[OpenRR] Mode`（`new` / `fast` / `legacy`）中实时无感切换（首次启动在后台编译 `New` / `Fast` 管线，就绪前暂以 `Legacy` 过渡）：

| 模式 | 1080p Quality | 1440p Quality | 4K Quality | 相对官方 `v0.1.7` 提速（`3×ABBA`） | 相对 RTX 5090 画质（47 帧均值 `PSNR_01` / Reinhard） |
|---|---|---|---|---|---|
| **`New`（默认推荐）** | `2.80 ms` | `4.79 ms` | `10.50 ms` | **`+6.4% ~ +8.2%`**（vs 官方 `Fp8=1` 仍快 `+5.3% ~ +6.9%`） | **`49.20 dB` / `54.29 dB`**（比官方 `v0.1.7` 高 `+3.0 dB`） |
| **`Fast`（实验性能档）** | `2.64 ms` | `4.59 ms` | `10.09 ms` | **`+7.7% ~ +9.2%`**（4K 帧时省 `0.93 ms`） | `48.44 dB` / `53.49 dB`（最深层跨帧复用，运动高亮处略敏感） |
| **`Legacy`（参考基准档）** | `3.66 ms` | `6.18 ms` | `13.47 ms` | 与上游 FP16 主路径 **`0-ULP` 逐位一致** | `46.96 dB` / `51.87 dB` |

- **显存占用不到 RTX 5090 一半**：单一生命周期复用池（`Engine::arena_`）使三档模式在 1080p Qual / 1440p Qual / 4K Perf / 4K Qual 下仅占 **`209 / 361 / 586 / 791 MB`** 显存——在画质与 RTX 5090 高度接近的前提下，**4K 显存比 RTX 5090 官方 NGX（常驻 `1.32 ~ 1.71 GiB`，含 DRS 峰值 `2.45 GiB`）立省 `0.7 ~ 1.1 GB`（节省 `48% ~ 70%`）**；原生 Vulkan（`RecordVk`）全直通下比上游原版额外再省 `60 ~ 130 MB`。
- 全 6 档分辨率预设、`84` 腿 `3×ABBA` 交错基准、三方显存明细及 `v1 + v2_motion` 画质真值对比，请参阅 **[v0.1.7plus 速度、画质与显存综合测评报告](docs/test-evidence/v017plus-speed-quality-eval.md)**（[English](docs/test-evidence/v017plus-speed-quality-eval.en.md)）。

### 2. 原生 OptiScaler DLSSD 提供者、状态恢复与多后端路由
- **原生 `OptiProvider_Dlssd` 接入**：不再在外部拦截游戏/Streamline 的 `NVSDK_NGX_*` 调用，而是直接通过 OptiScaler Plugin API v3 注册 `"OpenRR"` DLSSD 提供者。OptiScaler 负责上游游戏输入适配、Streamline `sl.dlss_d` 能力伪装以及下游 `OptiFG` / `XeFG` / `HUDfix` / `OutputScaling` / `RCAS` 编排；用户可在 OptiScaler `Upscalers -> RR Upscaler` 中实时切换提供者（`auto` 模式下 AMD 显卡自动优先选中 OpenRR）。
- **非阻塞能力探测**：首次查询仅做同步轻量检查（校验 `nvngx_dlssd.dll` 310.9.1 版本号 + 实例级 Vulkan 特性查询，带 30 秒失败缓存），将权重解析与管线编译全部放到 `CreateFeature` 后台线程，不阻塞游戏主线程。
- **D3D12 Direct 无侵入 Compute 状态恢复**：录制全程不调用 `SetDescriptorHeaps`、不触碰 Graphics Root 状态（描述符通过 Root Constant 裸地址直读）；在发现 D3D12 设备时通过 MinHook 挂钩 `D3D12Core` 底层函数（不触碰对象虚表），只读观测宿主 Heap 切换以剔除失效表句柄，并在 `Record` 后完整恢复游戏的 Compute Root Signature、全部 Compute Root 实参（Descriptor Table / CBV / SRV / UAV / 32-bit Root Constants）以及 PSO / State Object。原生支持《生化危机 9 (`re9.exe`)》《007 First Light》等游戏。
- **D3D12 ↔ Vulkan 桥接自动回退与硬化**：在无 `amdxc64.dll` ELF 直通的环境（如 Linux vkd3d-proton，或手动指定 `Backend = vulkan`）下自动回退至命令列表切分（Split）+ Vulkan 共享资源互操作桥接；补齐防自递归回退保护以及 Split Barrier / Enhanced Barrier / RTAS / MetaCommand 拒绝规则。
- **原生 Vulkan 游戏直通（`RecordVk`）**：通过 `LdrRegisterDllNotification` 监听晚加载的 `vulkan-1.dll`，在游戏 `vkCreateDevice` 时自动注入所需扩展、特性与独立计算队列，并在设备未就绪时自动拒绝创建避免崩溃。

### 3. 与 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 无缝共存
- 当 `OptiScaler\plugins\` 目录下同时存在 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 与 `openrr-amd.asi` 时，无需修改任何一方配置：OpenRR 会自动检测共存并启用 D3D12 Direct 模式（不叠加第二层命令列表 vtable hook），自动识别并追踪切分后的 continuation 命令列表。

### 4. 原生集成 OptiScaler `PLUGINS -> OpenRR` 标签页
- 通过 `OptiPluginInit` + `RegisterTab` 直接嵌入 OptiScaler 主菜单左侧 `PLUGINS` 栏，跟随 OptiScaler 的快捷键（默认 `Insert`）、主题与 DPI 缩放，彻底消除独立叠加层的 DX11/DX12/Vulkan/HDR 合成与按键冲突。
- 支持游戏内实时查看状态/双轨 GPU 耗时、切换 `New` / `Fast` / `Legacy` 网络模式、D3D12 后端、深度类型、各项附加引导图（Guides），点击 `Re-init Upscaler` 即刻触发 `ReInitUpscalers` 使后端/深度变更免重启生效；所有配置依然独立保存在 `openrr-amd.ini`。
- **零 Hook 多语言（含简中/日文/韩文）**：依托 OptiScaler nightly 的动态字形后端，在 `Language` 卡片中选择中文/日文/韩文后，可一键点击 `Configure CJK Font in OptiScaler.ini` 自动写入 `OptiScaler.ini` 的 `[Menu] UseHQFont=true` 与 `TTFFontPath`（优先指向随包附带的 `openrr-amd\lang\fonts\NotoSansCJKsc-Regular.ttf`，未找到时回退系统字体）；重启游戏后即正常显示 CJK 文字，配置生效前安全回退英文避免方块乱码。

### 5. 底层 `gpu/` 与 `runtime/` 修复（已独立提交，便于上游合入）
- **严格 GPU 退休证明与 Overflow 复用池**：移除按录制次数或超时猜测的槽位回收，改为结合 GPU 时间戳写回与游戏侧 `ID3D12CommandAllocator::Reset` / `VkCommandPool` 重置证明；64 个主槽位（自建列表独立占用 `kOwnSlot`）配合 64 个按需复用的 Overflow 槽池，满载时安全跳帧不抛异常。
- **录制期大缓冲 GPU 端填充与批量初始化**：修复 D3D12 绑定状态下 `fill` 超过 64 KB 上传环抛异常、以及 Vulkan 绑定状态下 2 参 `fill` 误重置游戏 `VkCommandBuffer` 的问题，统一改为 GPU 端 `clear_range.comp` 分段派发与 `vkCmdFillBuffer`，并在 `Engine::resize` 期间合并为单次批量提交。
- **双轨 GPU 计时、按需显存分配与残影消除**：同时记录并展示原始 GPU 耗时（`raw` last / p50 / p95）与过滤跨队列异步计算抢占后的估计耗时（`clean`）；Vulkan Host 输入/输出缓冲改为按需紧凑分配（全直通时省去 60~130 MB 显存）；暂停或打开游戏菜单超过 250 ms 返回时自动置位 `ONR_RR_FRAME_RESET` 消除切回瞬间的旧历史残影。

---

## 运行要求

- **OptiScaler 版本**：必须使用 **OptiScaler v0.10 nightly**（支持 Plugin API v3 `RegisterProvider` 与 `RegisterTab`）。
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

### 方式二：手动配合官方 OptiScaler v0.10 nightly（Windows / Linux 通用发行包）

> 从旧版本（`<= v0.1.7plus6`）升级前，请先删除游戏目录下旧版遗留的 `openrr-ngx-host.dll`（如曾安装），并清理 `OptiScaler.ini` 中手动设置的 `[Libraries] NvngxPath` 覆盖项；建议使用新版 `openrr-amd.ini`。

将构建产物（共 `392` 个文件的通用 `stage/` 包，或直接解压 `dist/OpenRR-v0.1.7.1plus-OptiScaler.zip`）放入游戏目录（即 OptiScaler 的 `dxgi.dll` / `OptiScaler.ini` 所在目录）：

| 文件 / 目录 | 放置位置（相对于 `OptiScaler.ini` 所在目录） |
|---|---|
| `openrr-amd.asi` | `OptiScaler\plugins\openrr-amd.asi` |
| `openrr-amd.ini` | `openrr-amd.ini` |
| `openrr-amd\shaders\*.spv` | `openrr-amd\shaders\*.spv`（共 `94` 个 `.spv`） |
| `openrr-amd\shaders-gfx11\*.spv` | `openrr-amd\shaders-gfx11\*.spv`（共 `94` 个 `.spv`，RDNA3 / RDNA3.5 必需） |
| `openrr-amd\aco\gfx1201\openrr-d3d12-v1\`、`openrr-amd\aco\gfx1200\openrr-d3d12-v1\` | 保持相同目录结构，每目标含 `94` 份 `.nrp`、`manifest.json` 和 `manifest.msgpack`；Windows RDNA4 严格 ACO 必需，Linux Mesa RADV 自动忽略 |
| `openrr-amd\lang\`、`openrr-amd\OpenRR_*.{txt,md}` | `openrr-amd\lang\`（多语言与 `fonts\`），以及包内随附的 `OpenRR_LICENSE.txt`、`OpenRR_NOTICE.txt`、`OpenRR_THIRD_PARTY.md` |
| `nvngx_dlssd.dll`（**310.9.1**） | 游戏目录或 `openrr-amd\nvngx_dlssd.dll` |

在 `OptiScaler.ini` 中开启 ASI 插件加载（OptiScaler 默认关闭该项）：

```ini
[Plugins]
LoadAsiPlugins=true
```

#### Linux（Wine / Proton + 系统 Mesa RADV >= 26.3）补充说明

- 使用同一份通用发行包即可直接在 Linux Wine / Proton 下运行：**只需系统安装 Mesa RADV >= 26.3**（无需打包或注入自定义 `libvulkan_radeon.so`，也无需 `with_mesa263.sh` / `VK_ICD_FILENAMES` 包装脚本）。
- 运行时优先通过 `winevulkan.dll` 解析 Vulkan 入口、使用 `vkd3d-proton` 原生 `RecordVk` 互操作，并在加载阶段对 `shaders/*.spv` 注入 `SpvDecorationNoContraction` 硬化直接交由系统 RADV 编译（自动跳过 Windows PAL 专用的 `aco\*.nrp` 离线记录）。
- 部署要点：游戏目录下除 OptiScaler v0.10 nightly（`dxgi.dll`、`OptiScaler.ini` 设 `LoadAsiPlugins=true`）与游戏自带或自备的 `nvngx_dlssd.dll`（**310.9.1**）外，仍需将 **`OptiScaler/plugins/openrr-amd.asi`**（注意 Linux 下 `OptiScaler` 必须保持首字母和 `S` 大写，才能与 OptiScaler v0.10 官方 `OptiScaler/plugins/` 目录合并）、`openrr-amd/`（`shaders/`、`shaders-gfx11/`、`lang/`）与 `openrr-amd.ini` 完整复制到游戏目录（不能只放单个 `.asi`；直接解压发布压缩包即可），并在 Steam 启动选项设置 `WINEDLLOVERRIDES="dxgi=n,b" %command%`。

> **重要提示**：
> 1. **切勿将插件改名为 `*-loadlate.asi`**：OptiScaler 会将 `loadlate` 插件放到独立线程并延迟 `LateAsiPluginsDelay`（默认 30 秒）再加载。届时游戏早已完成 NGX 能力查询与 `vkCreateDevice`，OpenRR 无法注册为 DLSSD 提供者或准备 Vulkan 设备。
> 2. **与 [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD) 共存**：将 `dlssnr-amd.asi` 与 `openrr-amd.asi` 直接同放在 `OptiScaler\plugins\` 下即可（均不要加 `loadlate`）。OpenRR 通过 OptiScaler 原生提供者通道接入并自动切换 D3D12 Direct 模式，无需改动 `dlssnr-amd` 的任何配置。

---

## 编译构建与打包（本地 / GitHub Actions）

需安装 `x86_64-w64-mingw32-g++`、`python3` 与 `glslangValidator`：

```bash
bash asi/build/build_asi.sh
```

构建完成后，打包目录位于 `build/asi/stage/`（分离出的调试符号保存在 `build/asi/openrr-amd.debug`，单元测试与命令列表切分验证套件位于 `build/asi/test_asi_units.exe`）；当存在双目标离线 ACO 清单或设置 `OPENRR_PACKAGE_ZIP=1` 时，会自动校验包结构、大小写与文件哈希并在 `dist/` 生成通用发行包 `dist/OpenRR-v<version>-OptiScaler.zip` 及 `.sha256` 校验文件（Windows 下也可直接运行 `python asi/build/package_release.py` 或 `asi/build/PACKAGE_LOCAL_RELEASE.cmd` 单独打包）。默认离线构建固定版本 Mesa 的 global/vector8 ACO 记录需要现有 WSL Mesa 源码与依赖，详见 [ACO 构建说明](asi/build/mesa/README.md)；云端 CI 通过 [`.github/workflows/package.yml`](.github/workflows/package.yml) 在 Ubuntu 26.04 容器中自动拉取 Mesa 26.3.0 / `libdrm` / Noto CJK 字体、完成完整构建与单测校验并上传或发布 Release 压缩包。stage 同时在 `openrr-amd/` 下包含 `OpenRR_LICENSE.txt`、`OpenRR_NOTICE.txt` 和 `OpenRR_THIRD_PARTY.md`，不包含 OptiScaler 或外部权重 DLL。

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

### 免费、禁止倒卖与二次开发署名要求
本项目**永久免费分发**，仅限个人、学习与科研等非商业用途。**严禁任何人在闲鱼、淘宝、QQ 群、付费网盘、收费整合包中售卖本插件或其编译产物，或以“付费远程代安装 / 付费调试 / 付费优化包”名义向他人收费提供。如果您是付费买到的，请立即申请退款并举报卖家！**

基于本项目的**非商业二次修改、重打包与衍生作品必须遵守 [NOTICE](NOTICE)**：二改作者可以署上自己的名字，但必须完整保留原始作者（`TheAutomatic`）署名、所有上游开源项目作者的版权声明、官方仓库链接以及全部 `Required Notice:` 行，**严禁抹除署名或仅修改二进制信息/版本号冒充原创**。

### 许可范围与上游归属
- **[本项目 (TheAutomatic / OpenRR-ASI)](https://github.com/TheAutomatic/OpenRR-ASI)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)**（见 [NOTICE](NOTICE)）：本分支的新增实现、独立 ASI 插件、构建/打包脚本与编译发行产物（`openrr-amd.asi`、预编译着色器/ACO 记录）。
- **[上游 OpenRR (OpenNeuralRendering/OpenRR)](https://github.com/OpenNeuralRendering/OpenRR)** — **MIT License**（© 2026 OpenNR）：核心 GPU 着色器、引擎、运行时与验证器（完整上游 MIT 许可正文保留于 [THIRD_PARTY.md](THIRD_PARTY.md)）。
- **其他上游与第三方组件**（`mochizuki0323/DLSSNR-AMD`、OptiScaler Plugin SDK、MinHook、Vulkan-Headers、Mesa RADV/ACO、Noto Sans CJK SC 等）各自保留原有开源许可，详见 [NOTICE](NOTICE) 与 [THIRD_PARTY.md](THIRD_PARTY.md)。

> 本仓库与发布包**不包含**任何 NVIDIA 专有二进制文件（如 `nvngx_dlssd.dll`）、提取的模型权重或 OptiScaler 二进制文件。用户需自行提供合法持有的 `nvngx_dlssd.dll`（`310.9.1`）。
