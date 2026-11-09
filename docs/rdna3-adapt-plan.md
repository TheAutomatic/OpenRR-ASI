# OpenRR RX 7000 / RDNA3（gfx11）适配计划

- 分支：`pr/openrr-amd-asi`，基于 HEAD `c671f57` / `bdf4188` 核对（源码导出在 box 的 `/workspace/openrr/src-c671f57/`，NUC 上已发布的 90 个 SPIR-V 在 `/workspace/openrr/stage-spv/`，NUC `vulkaninfo` 全量输出在 `/workspace/openrr/vkfull-nuc.txt`）。
- 本文仅作为备查技术方案与可行性评估存档，暂不安排实施，不改代码。
- 所有改动若未来实施均在 `pr/openrr-amd-asi` 这一条分支上做。`git worktree` 只当本地第二个检出目录用，不另开长期分叉的分支。

---

## 1. 现状：RDNA3 卡被拒有三道关，三道都要放开。

| 位置 | 现在做什么 |
|---|---|
| `runtime/src/runtime.cpp:1297-1302` `gpu_reason()` | 只要 `fp8_coopmat` 为 false，就返回 "not an RDNA4 GPU (RX 7000 and RX 6000 support comes later)"。 |
| `asi/src/rr_asi_main.cpp:944-947`、`957-960` | 没有 `VK_EXT_shader_float8` 扩展，或者 `shaderFloat8` / `shaderFloat8CooperativeMatrix` 为 false，就拒绝。 |
| `runtime/src/vk_host.cpp:94-99`、`117-133` | Vulkan 游戏路径：必需扩展里有 `VK_EXT_shader_float8`，必需特性里有 `shaderFloat8*`。 |
| `runtime/src/vk_host.cpp:222-224` | 建设备时无条件打开 `shaderFloat8` 和 `shaderFloat8CooperativeMatrix`。 |
| `gpu/src/vk.cpp:192`、`asi/src/rr_asi_main.cpp:908`、`915` | 没传 LUID 时只挑独显 AMD。纯核显机器（780M 掌机或笔记本）直接报 "no discrete AMD GPU found"。 |
| `runtime/bench/openrr_bench.cpp:370-380`、`runtime/validate/main.cpp:153` | bench 和 validate 只取高性能优先的第一块 AMD 卡。在 NUC 上永远选中 9070 XT，测不到 780M。 |

`Engine::set_fp8_ok` / `fp8_ok()`（`gpu/include/opennr_rr/engine.hpp:157-158`，`runtime.cpp:1063` 赋值）目前没有任何地方读取，所以「回退」现在是空的。

**放开这三道关之后也跑不起来，因为内核本身依赖 RDNA4。** 依赖分两层：

1. **FP8 类型。** 已发布的 90 个 SPIR-V 里，30 个声明了 `SPV_EXT_float8`（下面第 2 节有清单）。在不支持该扩展的设备上建管线是非法的。
2. **RDNA4 的寄存器布局。** 54 个网络内核全部用 Fast 主体：TR / TR2 / W1 / W2。这几种主体都依赖同一个前提：在 RDNA4 上，把累加器逐元素拷进 A/B 操作数，得到的正好是同一矩阵或其转置（`gpu/shaders/include/onr_block_fast.glsl:99-102`，`onr_block_w1.glsl:2-11`，`onr_block_w2.glsl:10-11`，验证内核是 `probe_wmma.comp:2-4`）。代码里有 `for i < h.length(): b[i] = h[i]` 这种拷贝（`onr_block_fast.glsl:178`、`705`），也有按 lane 位置做的 `subgroupShuffleXor(..., 16u)`（`onr_block_fast.glsl:773`，`onr_block_w1/w2` 多处）。
   - gfx11 上没有这个前提。wave32 下，A/B 每个 lane 持有 16 个元素，并且 lane i 和 i+16 必须是同一份副本。累加器每个 lane 只有 8 个元素，第 c 个元素是 (2c + l/16, l%16)。来源：AMD GPUOpen《How to accelerate AI applications on RDNA 3 using WMMA》（https://gpuopen.com/learn/wmma_on_rdna3/ ）；mochi 在 gfx1100 上的实测注释 `windows/shaders/rdna3/fswin_t.comp:163-184`。
   - 所以只加 `-DFP8QV=0` 不够。去掉 FP8 之后，这 54 个内核在 gfx11 上算出来仍然是错的。

## 2. FP8 / RDNA4 依赖清单

### 2.1 已发布的 SPIR-V 统计

扫描 NUC 上 `build/asi/stage/openrr-amd/shaders/` 的 90 个文件（10:59 构建），看 `OpExtension`：

- 54 个用到 `SPV_KHR_cooperative_matrix`，都是网络内核。
- 其中 30 个用到 `SPV_EXT_float8`。
- 其余 36 个不用 coopmat：`ae`、`post*`、`enc0_attr*`、`ingest`、`pack`、`downsample*`、`dfn_*`、`pooled_history2`、`post_tile`、`clear_range`、`selftest*`，以及对应的 `d3d_` 版本。这些在 gfx11 上原样可用，不需要第二份。

### 2.2 网络内核按主体分类

每一行包含 Legacy（`_f`）和 Fast（`_f_lt`），flow 层另有 `_fl` / `_fl_lt`。分类方法：在 box 上用 glslang 16.6.0 做预处理，看 `local_size` 和 `floate4m3_t`。

| 源文件 | 主体（选择宏） | FP8 来源 | 已发布文件数 | 在 RDNA3 上的替代 |
|---|---|---|---:|---|
| `enc0_stage_32_64` | W1（`onr_block.glsl:91`） | 无 | 4（含 `_f_w`） | **现成替代不能用**：行主体编译报 `#error`（`onr_block_fast.glsl:382-384`），因为 `enc0_attr_f` 写的 ATTR64 记录只有 W1 读得懂。要新写。 |
| `dec_up_32_64_64`、`dec_head_32_64` | W1 | 无 | 4 | 行主体（`-DW1=0 -DTR=0`）能编，但 LT_FAR / LT_Q16 会失效（见 3.4）。 |
| `enc_stage_64_64_96`、`dec_up_64_64_96` | W2（`onr_block.glsl:102-103`） | 无 | 4 | 行主体（`-DW2=0 -DTR=0`）能编。 |
| `dec_dfn_64_64` | W2 | `FP8QV`（`onr_block_w2.glsl:162-170`、`191-198`） | 2 | 行主体加 `-DFP8QV=0` 能编，0 处 FP8。 |
| `enc_stage_96_128_128`、`dec_dfn_96_128` | W2，并有 flow | `FP8QV` | 8 | 同上。 |
| `dec_up_96_128_128` | TR（SPLIT 1） | 无 | 4 | 行主体 `-DTR=0` 能编。 |
| `enc_stage_128_128_160`、`dec_dfn_128_128` | TR | `FP8QV`（`onr_block_fast.glsl:263-286`、`616-618`、`671-696`） | 8 | 行主体加 `-DFP8QV=0` 能编。 |
| `dec_up_128_128_160` | TR | 无 | 4 | 行主体能编。 |
| `enc_stage_160_256_160`、`enc_stage_160_256_0`、`dec_dfn_160_256` | TR2（SPLIT 2，`onr_block.glsl:83-84`、`264-269`） | `FP8QV` | 12 | 行主体加 `-DFP8QV=0 -DTR2=0` 能编。 |
| `dec_up_160_256_160` | TR2 | 无 | 4 | 行主体能编。 |
| **小计** | | | **54（FP8：30）** | **50 个能编，4 个（enc0_stage）要新写** |

以下几项不在已发布清单里，不需要处理：`FP8PROTO`（`onr_block_fast.glsl:184-207`，默认 0，只用于计时原型）、`enc_qv_fp8.comp`、`probe_rate_fp8*.comp`、`onr_probe_rate.glsl:7`。它们只在 harness 里用（`gpu/CMakeLists.txt:16` 的注释，以及 `asi/build/build_shaders.py:99-123` 的筛选规则）。

### 2.3 编译验证

在 box 上用 glslang 16.6.0 重跑 `build_shaders.py`，给全部网络内核加上 `-DFP8QV=0 -DTR=0 -DTR2=0 -DW1=0 -DW2=0`：

- 86/90 编译成功，产物里 0 处 `SPV_EXT_float8`。
- 失败的只有 4 个 `enc0_stage_32_64*`，原因是上面那个 `#error`。
- 对行主体做预处理检查：已经没有「累加器拷进 A/B」的写法，也没有按 lane 位置做的 coopmat 洗牌。剩下的都是同种矩阵之间的逐元素运算（同类型布局相同），以及针对 LDS 标量的 `subgroupShuffleXor(.., 1u)`。所以**行主体与布局无关，RDNA3 和 RDNA4 都能跑**。

### 2.4 权重和激活不用改格式

- 权重本来就以 fp16 存放：logical fp16(e4m3(w))，见 `onr_block.glsl:6`。FP8QV 用的 OCP e4m3 副本只是附加在 `in_a` / `in_b` 后面（`gpu/src/engine.cpp:175-193`，偏移是 `QV8_OFF`，`onr_block_fast.glsl:273-275`）。非 FP8 内核读不到这段，不读就行。想省显存的话，在 `fp8_ok()==false` 时可以不上传这段（in_a / in_b 少 1/3）。
- 激活 arena 存的是 e4m3 字节，由着色器里的整数运算解码成 fp16（`e4m3_in`，`onr_block.glsl:398-405`；`e4m3_lo/hi/x2`，`onr_block_fast.glsl:303-309`）。不依赖 FP8 硬件，因此**不需要** mochi 那种 FP16 孪生 arena（`windows/src/core/nr_graph.cpp:1353`、`nr_runtime.cpp:1904-1942`）。

## 3. 方案

### 3.1 用两个能力位，而不是一个 RDNA3 宏

FP8 和寄存器布局是两件独立的事，分开判断：

- **`fp8`**：直接用现有的 `info_.fp8_coopmat`（`gpu/src/vk.cpp:258`、`389`）。
- **`layout`**：取值 `rdna4` / `portable` / `rdna3`，决定寄存器主体（TR / W1 / W2）能不能用、按哪种布局写。
  - 判定方法：读 AMD 驱动编出的 ELF 里的 `EF_AMDGPU_MACH`；拿不到时退回 PCI ID。可以借鉴 mochi 的 `windows/src/core/nr_gpu_arch.hpp:30-56`、`213-250`，但要遵守 3.1b 的规则，不照搬它的直通路径。这个方法不受 OptiScaler 伪装 GPU 的影响（同文件 `:7-10`）。
  - 780M 的 deviceID 是 `0x1900`（`vkfull-nuc.txt:2228`），已在 mochi 的列表 `nr_gpu_arch.hpp:50` 里。

用户之前设想的 `#if !defined(RDNA3)`，改成按能力的宏：`ONR_FP8=0/1` 和 `ONR_LAYOUT=0/1/2`。阶段 2 / 3 新加的 FP8 矩阵乘都包在 `#if ONR_FP8` 里。

### 3.1b 架构识别的硬性规则（吸取 mochi 7800 XT 事故）

**mochi 的事故**：用户开了 OptiScaler 的 dxgi 伪装后，7800 XT 被当成 N 卡，进游戏直接没反应。mochi 的日志和代码里能看到两个原因：

- **补救查询本身也会被伪装**：直通路径 `adapter_arch()`（`public-0084/windows/src/pe/nr_d3d12_passthrough.cpp:287`）用 `GetDesc1()` 读显卡 ID。发现不是 AMD 后，它重新枚举显卡，但用的还是 `GetDesc1()`。伪装生效时，两次拿到的都是假 ID。
- **认不出就当 RDNA4 处理**：`on_device()`（同文件 `:356`）只对 rdna3 走单独分支，其余情况（包括 unknown）一律执行 gfx12 的自检和机器码。结果是 `CreateComputePipelineFromElf` 建管线成功，但自检 128 个值全部不匹配，随后才退回 Vulkan 桥接。

mochi 说这块改了很多次、每次都踩坑，所以 OpenRR **只借 ELF 判定这个思路，不照搬它的直通路径代码**，并按下面的规则执行：

1. **DXGI / D3D12 报告的 vendor 和 device ID 不参与选择内核。** 实际会遇到的伪装只有「A 卡伪装成 N 卡」，不会有人把 7000 伪装成 9000。所以只要报告的是 NVIDIA 或别的厂商，就**忽略它，继续往下判定**，不能当成「不是 AMD」直接放弃，也不能因此降级。
2. **以驱动结论为准，按游戏用的图形 API 分两条路径判定，每条都是拿到第一个结果就直接用，不交叉比对：**

   **DX12 游戏**（走 D3D12 直通或 D3D12↔Vulkan 桥接）：
   - ① 驱动实际编出的 ELF 里的 `EF_AMDGPU_MACH`（ELF Header `e_flags & 0xff`，取值按 LLVM `llvm/include/llvm/BinaryFormat/ELF.h` 的 `EF_AMDGPU_MACH_AMDGCN_*` 表**逐个白名单**：gfx11 = `0x41` gfx1100、`0x46` gfx1101、`0x47` gfx1102、`0x44` gfx1103（780M）、`0x43` gfx1150、`0x4a` gfx1151、`0x55` gfx1152、`0x58` gfx1153；gfx12 = `0x48` gfx1200、`0x4e` gfx1201（9070 XT，NUC 上 `%LOCALAPPDATA%\OpenNR\rr\elf-*\*.elf` 实测 `e_flags = 0x4e`）。`0x42` 是 gfx1013（RDNA1），不是 gfx11；表外的值一律按 unknown 处理）。这是驱动按真实硬件写入的，伪装改不到。gfx12 → RDNA4 内核集；gfx11 → RDNA3 / portable 集。**拿到它就直接用**，所以不会误伤 9000 系。注意：在 OpenRR 中，ELF 是由独立 Vulkan 设备 `c->dev.pipeline_elf()` 通过 `VK_KHR_pipeline_executable_properties` 从 AMD Vulkan 驱动取出的（[`gpu/src/vk.cpp:662-711`](../gpu/src/vk.cpp#L662-L711)：取名字以 `ELF` 开头的 internal representation，从 `0x7f 'E' 'L' 'F'` 处截取，`:697-702`；只在 `stats_` 为真时可用，`:233`、`:663`），D3D12 扩展 `CreateComputePipelineFromElf` 只是消费方（`d3d12.cpp:860`、`897`）。DX12 游戏不论最后走 D3D12 直通还是 D3D12↔Vulkan 桥接，`Create` 都会先按游戏适配器的 LUID 建 `c->dev`（`runtime.cpp:1416-1422`，早于后端选择），所以两条子路径都可以在 `c->dev` 建好后直接编译 `selftest.spv` 取 ELF 判定，不依赖 `amdxc64.dll` 直通成功。注意要直接调 `c->dev.pipeline_elf()`，**不要走 `D3dDevice::elf_of()` 的磁盘缓存**：缓存键（`d3d12.cpp:316-317`）含 Vulkan 设备名、不含 LUID，设备名可能被伪装，核显和独显又是同一个驱动版本，键可能撞上，读到另一块卡的 ELF。
   - ② 拿不到 ELF 时（例如驱动未暴露 `VK_KHR_pipeline_executable_properties`），用 Vulkan 能力位，判法同下面 Vulkan 游戏的 ①。桥接用的 `VkPhysicalDevice` 按 LUID 对应到游戏的 D3D12 适配器。

   **Vulkan 游戏**（采用宿主 `VkDevice`）：
   - ① 对游戏或宿主给的 `VkPhysicalDevice` 查能力位：`shaderFloat8CooperativeMatrix` 为真 → RDNA4 集；`cooperativeMatrix` 为真、`shaderFloat8CooperativeMatrix` 为假 → RDNA3 / portable 集。能力位反映的是驱动的真实能力，DXGI 伪装碰不到；OptiScaler 的 Vulkan 伪装最多改 `vendorID` / `deviceID` 和设备名，也改不了特性。用 `vkGetPhysicalDeviceFeatures2` 直接查，不要沿用宿主缓存的结果。
   - 注意：`VkPhysicalDeviceProperties` 里的 `vendorID` / `deviceID` / `deviceName` 可能被伪装，**不能当主要依据**。
   - 可选的加强手段：如果宿主设备支持 `VK_KHR_pipeline_executable_properties`（在 `PrepareVkDevice` 请求打开 `pipelineExecutableInfo`），可以从 `selftest.spv` 管线里读 ELF `EF_AMDGPU_MACH` 或统计信息。**只用于写日志，不参与决策**（与上面「不交叉比对」一致）。注意现在宿主设备上取不到：`Device::adopt()`（`vk.cpp:361`）不设 `stats_`，`pipeline_elf()` 直接返回空（`:663`）；要用的话 `PrepareVkDevice` 要加这个扩展，`adopt()` 也要设 `stats_`。

   **两条路径共用的最后手段**：③ PCI ID，只在前面都拿不到时才用（比如 Vulkan 实例建不起来）。要取 SetupAPI / CfgMgr 读到的硬件 ID，不能取 DXGI 或 `VkPhysicalDeviceProperties` 的。

3. **只有当前路径的所有手段（含 PCI ID）都拿不到时才算 unknown，unknown 走 portable 集，绝不默认 RDNA4。** mochi 出事的根源就是 unknown 默认走了 gfx12，而 OpenRR 按上面的顺序，9070 XT 在任一路径的前两步就能确定，不会落到 unknown。
4. **自检只兜底，不推翻判定。** 自检失败时，这块卡在本次进程里改用 portable 集，并在日志里写清「架构 X，来源 ELF / Vulkan 能力位 / PCI ID，自检失败」，避免反复建管线、反复失败。自检通过就照常满血运行，不额外降级。
5. **失败要明确报错。** 任何一步拒绝或回退，都要写日志并返回明确错误，让 OptiScaler 能退回自己的路径，不能出现游戏在跑、画面没反应的情况。

**验证**（并入第 5 节）：

- 在 780M 和 9070 XT 上各开一次 OptiScaler 的 dxgi 伪装（伪装成 N 卡），确认识别结果与不开伪装时**完全相同**：9070 XT 仍然加载 RDNA4 集，满血运行；780M 仍然走 RDNA3 / portable 集。日志里写明架构是从哪条路径、哪一步取到的（ELF、Vulkan 能力位或 PCI ID）。
- Vulkan 路径单独验证：用一个 Vulkan 游戏或 `openrr_bench` 的 Vulkan 后端，在 9070 XT 和 780M 上各跑一次，开启 OptiScaler 的 Vulkan 伪装（改 `vendorID`/`deviceID`），确认仍按能力位选对内核集，日志里写明来源是「Vulkan 能力位」。
- 用一个测试开关强制架构为 unknown，确认不会加载任何 gfx12 机器码。

### 3.2 内核集：一份源码，两套产物

| 内核集 | 宏 | 目录 | 谁用 |
|---|---|---|---|
| `rdna4`（现有，默认） | 不变 | `shaders/` | `fp8 && layout==rdna4` |
| `portable` | 网络内核加 `-DFP8QV=0 -DTR=0 -DTR2=0 -DW1=0 -DW2=0`，再加 3.4 的补丁 | `shaders/portable/`，只放 54 个网络内核 | 其他所有情况，或者用环境变量 `ONR_RR_KSET=portable` 强制 |
| `rdna3`（可选，见第 6 节） | gfx11 的寄存器主体 | `shaders/rdna3/` | `layout==rdna3`，而且该集在 780M 上验证通过 |

- 构建：`asi/build/build_shaders.py:66-123` 和 `gpu/CMakeLists.txt:79-121` 各加一轮。网络内核（`^(enc_stage_|enc0_stage_|dec_)`）带 portable 宏，再输出一遍到子目录。非 coopmat 的 36 个不复制。
- 加载：`gpu/src/kernels.cpp:16-34` 和 `runtime.cpp:527-568`、`588` 加一个子目录参数。按名字找不到时先在子目录找，再退回 `shaders/`。
- D3D12 路径（`d3d12.cpp:844-851`）的 ELF 缓存键包含 SPIR-V 内容哈希和驱动键，两套内核不会串。`d3d_` 前缀判断看的是文件名（`vk.cpp:634`），放进子目录不影响。
- `Engine::set_fp8_ok`（`engine.hpp:157`）真正用起来：由它和 `layout` 一起决定 `Kernels` 读哪个目录。
- 用户原来的计划是「另开 worktree，从基线切一条 RDNA3 线」。这里改成同一分支、同一套源码出两份产物，原因是：
  - 网络内核共用 `onr_block*.glsl`。B0–B2 以及阶段 2 / 3 每次改 include，分叉的线都要手工同步。
  - 用同一份源码，portable 集在 9070 XT 上就能测（见第 5 节）。
  - worktree 可以留着当本地第二个检出目录，比如在 780M 上跑测试时用，但代码只在 `pr/openrr-amd-asi` 上提交。

### 3.3 设备特性改为按需打开

- `runtime/src/vk_host.cpp`：
  - `:94-95` 的必需扩展里去掉 `VK_EXT_shader_float8`。
  - `:126` 的必需特性里去掉 `shaderFloat8*`。
  - `:222-224` 改成仅在支持时打开，写法和同文件 `:227-229` 处理 `8BitAccess` 一样。
  - 拒绝文案改成「缺 cooperativeMatrix」，不再写「需要 RDNA4」。
- `gpu/src/vk.cpp:212-213` 的查询链已经允许 FP8 结构体缺省（`has_f8`，`:258`），只需确认建设备时不强开 FP8。mochi 的写法可以参考：`windows/src/core/nr_device_features.hpp:47-71` 和 `nrvk.hpp:204`。
- `runtime.cpp:1297-1302` 和 `rr_asi_main.cpp:940-960` 改成：
  - 没有 coopmat：拒绝。
  - 没有 FP8，且 portable 集不全：拒绝，日志里写明缺哪些文件。
  - 其余情况：放行，日志写出所选内核集。
- `gpu/src/vk.cpp:192`、`rr_asi_main.cpp:908`：没传 LUID 时先找独显，找不到再接受 AMD 核显（`VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`）。
- `asi/src/rr_pe_config.cpp:128` 里的 "RX 9000 / RDNA4" 文案一起更新。

### 3.4 portable 集上要修补的四处着色器问题（含行主体隐蔽 Bug）

1. **`enc0_stage_32_64` 的嵌入层。** 行主体要能读 ATTR64 记录（`enc0_attr.comp:329`、`349`、`551`；读法参考 `onr_block_w1.glsl:162-182`）。另一个办法是 portable 集用一个非 ATTR64 的 `enc0_attr_f` 变体，但这样 `engine.cpp:458` 的 attr 缓冲尺寸也要跟着变。优先做第一种：让行主体在 `ENC_EMBED && ENC_F32` 下按 `EMB_REC` + `UNREC` 解包 48/64 字节记录写入 `lds[pr + ...]`，这样 attr 缓冲和 B1 `t0` 尾部池化布局都保持不变。
2. **`dec_head_32_64` 行主体的空指针写越界（必崩 Bug）。** 在 [`gpu/src/engine.cpp:723`](../gpu/src/engine.cpp#L723) 中，当融合 `post_tile` 生效且不导出 DFN 时（`fast_ && p.tile && !dfn_out`），宿主会将 `p.dfn_out` 传为 `0`（GPU 虚拟地址 `0x0`）。这里的 `fast_` 是 `_f` 运行时内核的开关，默认为真（`engine.hpp:209`），不是用户的 Fast 模式，所以 Legacy 和 Fast 都走这个分支；portable 集主机代码不变时同样会传 0。`W1` 主体（[`onr_block_w1.glsl:36-42,65-68`](../gpu/shaders/include/onr_block_w1.glsl#L36-L68)）检查了 `if (uint64_t(pc.dfn_out) != 0ul)`（`:37`），在 `tl` 为真时跳过写 `pc.ajab`（`:42`），`pc.aniso` 只在 `LT_HM` 下跳过（`:65-67`）；而 `onr_block_fast.glsl:1265` 的非 `W1` 路径直接执行 `F16x4Buf(uint64_t(pc.dfn_out)).v[pi >> 2] = o;`，向地址 `0x0` 写入会立即触发 GPU Page Fault / TDR。必须将 `onr_block_w1.glsl:36-42,65-68` 的 `pc.dfn_out != 0ul` 与 `!tl` 保护同步到 `onr_block_fast.glsl:1265-1297`。`ajab` / `aniso` 在融合时写了也无害（`engine.cpp:490-493`，它们是 `attr_` 头部的独立区域），同步 `!tl` 只为和 W1 一致。另外，`pc.live_h` 只有 W1 实现（`onr_block.glsl:216`、`onr_block_w1.glsl:97`），行主体的 `dec_up_32_64_64` 会多算 `live_h` 以下的窗口：这是性能问题，不会崩，可以在 `onr_block_fast.glsl:404-407` 旁按同样条件补提前返回。
3. **Fast 模式宏在行主体中的缺失与硬编码遗漏（`LT_FAR` / `LT_Q16` / `LT_RCP` / `LT_HM` / `LT_HID`）。**
   - `LT_FAR` / `LT_Q16`：只在 TR / W1 主体里实现（`IFK/TK` 在 `onr_block_fast.glsl:749-765` 和 `onr_block_w1.glsl:233-270`；`LT_Q16` 只在 W1 里起作用，见 `onr_block.glsl:131-139`）。预处理检查结果：`dec_up_32_64_64` 在 `-DTR=0 -DW1=0` 下 `LT_FAR` 命中数为 0，需要移植进行主体的 S / PV 循环。
   - `LT_RCP`：行主体 softmax（`onr_block_fast.glsl:1057`）硬编码了 `float16_t(rcp_rn(float(sum)))`，漏用了 `RCP_SUM(sum)`，导致 `fastmode.cmake` 里带 `LT_RCP=1` 的 19 个 `_lt` 内核在行主体下静默丢掉这一项（TR 在 `:779`、W1/W2 在 `onr_block_w1.glsl:258`、`onr_block_w2.glsl:273` 用的都是 `RCP_SUM`）。
   - `LT_HM`：`onr_block_fast.glsl:1280,1283,1288,1340` 硬编码了 `half_rz` 与 `rcp_rn`，未像 `W1/W2` 那样使用 `HEAD_RZ` 与 `SIG_RCP`。`LT_HM` 不是整体失效：`sigmoid16`、`unorm8_code`、`snorm8_code`、`tanh_f32` 里的 `LT_HM` 分支照常生效（`onr_block.glsl:461-470`、`546-570`），漏掉的只是这 4 处内联写法和 dec_head 的 `aniso` 跳写。**`:1340` 是 TR 和行主体共用的 `ENC_STAGE` DFN 代码**：RDNA4 上 `enc_stage_128_128_160`、`enc_stage_160_256_*` 走 TR，今天就在用 `rcp_rn`。直接改成 `SIG_RCP` 会改变 RDNA4 的 Fast 输出，所以只能在行主体分支里改（例如 `#if !TR`）；portable 集里受影响的是 `enc0_stage_32_64`、`enc_stage_64_64_96`、`enc_stage_96_128_128`（RDNA4 上分别走 W1 / W2，用 `SIG_RCP`，`onr_block_w1.glsl:335`、`onr_block_w2.glsl:399`）。
   - `LT_HID`：行主体 `#if MLP64` 分支（`onr_block_fast.glsl:1125,1152`）循环上界写成了 `j < HID` 而非 `j < LT_HID`（TR 在 `:866`、行主体非 MLP64 分支在 `:1177` 用的是 `LT_HID`）。目前没有任何内核定义 `LT_HID`（`fastmode.cmake` 里没有），它默认等于 `HID`（`onr_block.glsl:118-119`），所以现在没有实际影响，只是一致性问题。
4. **LDS 用量超 `32 KB` 上限。** 行主体的 LDS 比现在大很多（从 SPIR-V 的 Workgroup 变量算出）：

   | 内核 | 现在 | portable（原始行主体） |
   |---|---:|---:|
   | `dec_up_32_64_64` | 4 KB | 21 KB |
   | `enc_stage_96_128_128` | 12 KB | 37 KB |
   | `enc_stage_160_*` 等 4 个 C=160 内核 | 29.5 KB | 49 KB |

   - 9 个 portable 内核超过了驱动报告的 `maxComputeSharedMemorySize = 32768`：780M 见 `vkfull-nuc.txt:2286`，9070 XT 见 `:308`。这是 AMD Windows 驱动在这两块卡上实测报的值，OpenRR 只在 Windows 上跑，所以 32 KB 就是要满足的上限。现有 RDNA4 内核最大正好 32768（`dec_up_128_128_160`）。
   - 注意：在 [`onr_block.glsl:352`](../gpu/shaders/include/onr_block.glsl#L352) 中，`SCR` 与 `QV+PR` **已经是叠放关系**（`PR = QV + max(4096u * QV_BUFS + 64u * SST, SCR) - 64u * SST`）。仅设 `QV_BUFS = 1` 只能把 `C = 96` 压到 `29 KB`（`14,848` halves），但 `C = 160`（`SPLIT = 2`）因为 `XS` 常驻占 `64 * 160 = 10,240` halves（`20 KB`），加上 `QV`（`4,096` halves）+ `PR`（`4,608` halves）+ `OB`（`2,048` halves）或 `SCR`（`10,240` halves），仍高达 **`41–44 KB`**（`20,992–22,528` halves），依然超过 `32 KB`！
   - 要把 `C = 160` 行主体真正压进 `32,768` 字节（`16,384` halves），需要组合三步：
     1. 设 `QV_BUFS = 1`（省 `4,096` halves）；
     2. 在注意力循环内，当 `S = Q_h Q_h^T` 在寄存器中算完并 `WG_SYNC()` 后，`Q_h`（`qb`，`2,048` halves）生命周期已结束，将 `PR` 叠放到 `qb` 起始位置，并将 `SPLIT = 2` 的 `OB`（`2,048` halves）叠放进 `PR`/`vb`（合省 `4,096` halves）；同时在 `ENC_C >= 160` 时将 `SST` 从 `72u` 设为 `64u`（与 `onr_block.glsl:377-380` 参考路径一致，省 `512` halves），此时注意力 + MLP 期间 LDS 恰好为 `10240 (XS) + 2048 (vb) + 4096 (PR) = 16,384` halves = **精确 `32,768` 字节（`32 KB`）**。实现时要注意三点：(a) SST=64 时 `PR` 是 4,096 halves，比 `qb` 大一倍，所以布局必须是 `XS | vb | qb+后续 2,048`，`vb` 放在 `qb` 前面，否则 `PR` 会盖掉 P·V 还要读的 `V_h`；(b) 现在 S 的 `coopMatStore` 前没有工作组屏障（`onr_block_fast.glsl:989-1000`，各子组还在读全部 `qb` 当 key），叠放后要加一次 `WG_SYNC()`；`OB` 叠进 `PR` 前也要再加一次，因为同一行块的两个半组 `hf` 都在读同一段 P（`:1064-1077`）；(c) SST 从 72 改成 64 会去掉 softmax 行的填充（`onr_block.glsl:309` 的 SST=72；`:376` 参考路径注释说明这个填充是把 32 路 LDS bank 冲突降到 2 路），性能要实测；
     3. 对 `ENC_CO > 0`（`enc_stage_160_256_160`）和 `DEC_UP`（`dec_up_160_256_160`），取消 `64 * ENC_C`（`10,240` halves）的全量 `SCR`：在 `DEC_UP` 中将 `UPLO`（`2,560` halves）放在 `10240..12800`，`UPU` 直接写在 `XS`（`0..10240`）并通过寄存器暂存 + `WG_SYNC()` 原地完成 `z = fma(skip, sc, U)` 重排；在 `ENC_CO > 0` 的 merge 中按 tile 分两批通过 `QV+PR`（`6,144` halves）重排，不再同时驻留两份 `[64][160]`。

### 3.5 Legacy 和 Fast 都要有

运行时只发布 `_f`（Legacy，`--fast-mode 0`）和 `_f_lt`（Fast，`Engine::set_lite`，`runtime.cpp:400`、`1078-1081`）两种网络内核。portable 集两种都要输出，共 54 个文件。不发布的 `_x` / `_e` / 无后缀 k8 参考内核不用管（`gpu/CMakeLists.txt:79-103`）。

### 3.6 flow 链（`_fl`）

- `onr_flow.glsl:1-8` 靠「同队列内前一个 dispatch 的窗口先全部发射」来保证不会死锁。这对 gfx11 同样成立，但 780M 只有 12 个 CU（公开规格），每个窗口占用的 LDS 也更大，等待时间会变长。
- mochi 在 RDNA3 上把类似的分块链和常驻工作组默认关掉了，原因是 ≥1080p 时会 TDR（`windows/src/core/nr_graph.cpp:172-182`、`4190-4193`）。它的机制是常驻工作组，和 OpenRR 的行标志不同，但仍是一个风险信号。
- portable 集先默认 `set_flow(false)`（`ONR_RR_FLOW=0` 已经能强制关闭，见 `runtime.cpp:853-856`）。在 780M 上 4K 下 flow 开关两种都测过没有挂起，再打开。

## 4. 借鉴 mochi

所有引用对应 `/workspace/mochi-gitlab`（GitLab 上游 `1336d50`，2026-10-31）。

**可以借鉴：**

| 内容 | 位置 | 在 OpenRR 里怎么用 |
|---|---|---|
| 架构判定：驱动编出的 ELF 里的 `EF_AMDGPU_MACH`，退路是 PCI ID，不受 OptiScaler 伪装影响 | `windows/src/core/nr_gpu_arch.hpp:1-56`、`213-250`；machine 值对照 `nr_pal_binary.hpp:181-182` | 3.1 的 `layout` 判定。OpenRR 的 D3D12 路径本来就拿 LLPC 的 ELF（`d3d12.cpp:844-877`），可以直接读 ELF 头。 |
| 按目标决定是否打开 FP8 设备特性 | `nr_device_features.hpp:47-71`、`nrvk.hpp:62-104` | 3.3 |
| 每种架构一个着色器目录，由运行时选择 | `windows/shaders/rdna3/`、`rdna4/`，`nr_gpu_arch.hpp:27-28` | 3.2 的子目录方案 |
| gfx11 的 fragment 布局实测结论，以及「累加器 → 操作数」用 `subgroupShuffleXor(.,16)` 交换半波 | `windows/shaders/rdna3/fswin_t.comp:163-227`（`NR_OPPUT_X` / `NR_OPLIN`） | 第 6 节的 `rdna3` 寄存器主体（TR / W1 / W2 移植） |
| k-pair swap：上半波的操作数每对 k 互换，权重奇数行也同样互换，省掉一次选择 | `fswin_t.comp:229-275`；权重重排 `nr_graph.cpp:2181-2187`、`4099-4173` | `rdna3` 寄存器主体的进一步优化（可选）。权重重排放在 `engine.cpp` 的 `with_tiles`（`:195-203`）一侧。 |
| 用 `coopMatLoad` 直接把偏置等加数读进累加器，不依赖元素坐标 | `windows/shaders/rdna3/include/coopmm.glsl:96-109` | 行主体里的偏置已经这样做了（`bias_acc`，`onr_block.glsl:580-584`），确认一下即可。 |
| 在 RDNA3 上默认关掉跨 dispatch 链 / 常驻循环，通过环境变量打开 | `nr_graph.cpp:172-182`、`4190-4193` | 3.6 |
| 用于测占用率的 LDS 填充手段 | `windows/shaders/rdna3/include/occ_pad.glsl` | OpenRR 已经有 `LDS_OCC_PAD`；portable 集里不要默认打开，按需实测。 |

**不能照搬：**

- **mochi 的内核本身。** mochi 跑的是 `nvngx_dlssnr.dll` 的 NR transformer（Swin / ViT），OpenRR 跑的是 `nvngx_dlssd.dll` 的 RR 编解码器加 DFN，两边的网络和内核没有对应关系。
- **ACO 机器码路线。** mochi 在 Windows 上的 RDNA3 只靠预编译的 Mesa ACO 机器码（通过 `VK_KHR_pipeline_binary` 导入）运行。没有 ACO 时 RDNA3「不跑 NR」（`README.md:408-415`；`windows/src/core/nr_runtime.cpp:1055-1065` 会直接抛错）。它的 gfx11 布局结论是在 RADV / ACO 上测的（`fswin_t.comp:165`、`229-234`）。OpenRR 的 Vulkan 和 D3D12 两条路径都用 AMD 自己的 LLPC 编译器（`d3d12.cpp:860`）。KHR coopmat 的元素映射由实现决定（`coopmm.glsl:46-48`），**LLPC 在 gfx11 上的映射必须先在 780M 上实测**。mochi 一整套 ACO 预编译、导入和模板外壳的基础设施工作量很大，本计划不采用。
- **FP16 孪生 arena 和 e4m3 软件量化**（`nr_graph.cpp:1353`、`windows/shaders/rdna3/include/e4m3_emul.glsl`）。OpenRR 的激活本来就是 e4m3 字节，在着色器里整数解码（2.4），用不上。
- **mochi 在 RDNA3 上的性能数据。** README 没有给 RDNA3 的数字，自己写的是 "experimental … may not match the stability and performance of DLSSNR-RDNA3"（`README.md:36-41`、`417-422`），不能拿来推算 OpenRR。

## 5. 验证

在 9070 XT 上能做的都先在 9070 XT 上做，780M 只用来确认 gfx11 特有的问题。

**9070 XT（RDNA4）：**

1. RDNA4 默认输出必须与 `c671f57` 逐位一致：`openrr_bench` Legacy 和 Fast 都要求 `max_ulp_diff = 0`。证明默认集一字节都没变，这与基线门禁的写法一致。另外用 K2 那种 SPIR-V 哈希门禁（`asi/tests/test_asi_units.cpp:621-646`）断言默认 `shaders/` 的哈希不变。
2. `ONR_RR_KSET=portable` 强制使用 portable 集，3 个分辨率全部满足：
   - 所有内核都能建出管线；
   - `non_finite = 0`；
   - 对 RDNA4 默认输出报告 `PSNR_01`、`PSNR_tonemap`、`p99_abs_err`、`max_abs_err`（与 bench 现有口径相同，见 `openrr_bench.cpp:680-738`）；
   - 阈值钉在实测值附近。
3. 预期的差异来源：
   - FP8QV 关掉后，Q/V 用 fp16 精确值代替 OCP 次正规映射（`onr_block_fast.glsl:254-257`），Legacy 会比现在更接近参考；
   - 3.4 第 3 项补齐之前，Fast 会少 `LT_FAR` / `LT_Q16` / `LT_RCP` 和部分 `LT_HM` 近似。
   这两项要分别单独开关测，确认差异来自哪里。
4. portable 集在 9070 XT 上的性能（同进程 A/B）：作为「RDNA4 寄存器主体比行主体快多少」的参照，也用来给第 6 节做估算。

**780M（RDNA3，gfx1103）：**

5. 先跑探针：
   - `probe_wmma`（`gpu/harness/probe.cpp:196-257`），把 LLPC 下累加器 / A / B 的 (lane, element) → (row, col) 映射记进证据日志；
   - 新加一次 `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` 枚举，打印支持的 (M, N, K, A/B/C 类型)。
   - harness、bench、validate 需要先加一个按 LUID 或序号选卡的参数（`vk.cpp:191-197`、`openrr_bench.cpp:370-380`、`validate/main.cpp:153`）。
6. 开 validation layer，加载 portable 集的全部内核，确认 LDS 超过 32 KB 的那 9 个（3.4 第 4 项）是否报错。
7. Legacy 和 Fast 各跑 1080p 和 4K，各 20 帧：
   - `non_finite = 0`；
   - 同一输入下，与 9070 XT 上 portable 集的输出比 PSNR（两边内核相同，只有硬件求和顺序可能不同，预期非常接近）；
   - 与 9070 XT 默认集的输出比 PSNR。
8. flow 开关两种各跑 4K 200 帧，确认没有挂起或 TDR（3.6）。
9. D3D12 路径：`openrr_bench`（D3D12）在 780M 上能建出 ELF 管线并跑完。`patch_elf`（`d3d12.cpp:100-162`）和 AMD 扩展 `CreateComputePipelineFromElf` 在 gfx11 上都还没跑过。
10. 记录性能数字，但不设门槛。780M 的规模（12 CU、共享系统内存）决定了它只能用来做正确性验证（估计：1080p 会比 9070 XT 慢一个数量级）。真实的 RX 7000 性能要等有独显时再测。

## 6. 实施步骤拆解与风险评估

### 6.1 任务拆解清单

| # | 步骤 | 复杂度与风险等级 |
|---|---|---|
| 1 | 选卡参数，780M 上跑 `probe_wmma` 和 coopmat 属性枚举 | 低风险 |
| 2 | 设备特性按需打开，三道拒绝改成按能力判断，核显可选 | 低风险 |
| 3 | 构建第二套内核，运行时按 `fp8` / `layout` / `ONR_RR_KSET` 选目录 | 低风险 |
| 4 | 行主体支持 `enc0` 的 `ATTR64` 嵌入与 `dec_head` 空指针写保护（3.4 第 1、2 项） | 中等（局部着色器改动） |
| 5 | `LT_FAR` / `LT_Q16` / `LT_RCP` / `LT_HM` / `LT_HID` 补齐进行主体（3.4 第 3 项） | 中等 |
| 6 | `C=96` / `C=160` 行主体 LDS 压进 32 KB 以内（3.4 第 4 项） | 中等（需在 9070 XT 上核对逐位/PSNR） |
| 7 | 9070 XT 和 780M 上的完整验证（第 5 节） | 低风险 |
| 8 | （可选）gfx11 寄存器主体：TR / TR2 / W1 / W2 按 LLPC 实测的布局改写「累加器 → 操作数」交换，可选 k-pair swap 和权重重排 | 高复杂度、高风险（强依赖 LLPC 内部实现） |

### 6.2 最大的风险

1. LLPC 在 gfx11 上的 coopmat 元素映射还不知道（第 4 节）。portable 集不受它影响，第 8 步完全取决于它。
2. 行主体在 RDNA4 上从来没有作为发布路径跑过，存在历史遗留未同步缺陷（例如 3.4 第 2 项已查出的 `dec_head_32_64` `pc.dfn_out == 0` 空指针写越界，以及 3.4 第 3 项查出的 `LT_RCP` / `LT_HM` / `LT_HID` 漏接）。好在这些问题可以先在 9070 XT 上通过 `ONR_RR_KSET=portable` 全部抓出来并修干净。
3. LDS 超过驱动报告的 32 KB 上限（3.4 第 4 项已给出精确压到 `32,768` 字节的叠放方案）。
4. D3D12 ELF 路径在 gfx11 上没有验证过（第 5 节第 9 步）。
5. 性能。行主体每个 token 都要走 LDS，LDS 用量也大 2–5 倍，所以 RX 7000 上的速度只能实测评估（7900 XTX 的 FP16 WMMA 峰值按公开规格约为 9070 XT 的 60%，再加上行主体本身的 LDS 往返与占用率开销，帧耗时会显著高于 9070 XT）。

### 6.3 方案总结

- **第一阶段（`portable` 集，步骤 1–7）可行性高、边界清晰**：不碰 RDNA4 默认输出，绝大部分逻辑与正确性在 9070 XT 上就能通过 `ONR_RR_KSET=portable` 跑通验证。
- **第二阶段（`gfx11` 专属寄存器主体，步骤 8）性价比低、维护负担重**：需把 `TR` / `TR2` / `W1` / `W2` 四套主体按 LLPC 的 `gfx11` WMMA 寄存器排布重写半波交换与 k-pair 重排，仅在未来实测 `portable` 集在 RX 7000 独显上性能确实不可接受时才值得考虑。

---

### 附：没核实的项

- AMD Windows 驱动 26.10.1 在 780M 上实际支持的 coopmat 配置列表：`vulkaninfo` 不打印，要在第 5 节第 5 步测。
- LLPC 在 gfx11 上的 fragment 元素映射：同上。
- 超过 32 KB 的 LDS 在 AMD Windows 驱动上能不能用：要在第 5 节第 6 步测（或直接按 3.4 第 4 项压到 32 KB）。
- 行主体（`TR=0`）修补 3.4 第 1–4 项后在 RDNA4 上的数值正确性：代码注释说它和 TR 主体的运算顺序一致（`onr_block_fast.glsl:99-102`），待实际跑通验证。
