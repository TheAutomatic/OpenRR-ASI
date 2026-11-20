# AMD 光追提速边界、ACO 借鉴可行性与 RDNA4 实测经验

记录时间：2026-11-20。基线为 OpenRR `v0.1.7plus`（`e337993`，`asi-plugin` 分支）。
本文件整理围绕“能否像 OpenRR 加速光线重建那样加速 AMD 光追本身”的架构分析，以及基于 RX 9070 XT（RDNA4 / `gfx1201`）在 RTX Remix 路径追踪中完成 7 轮单变量实测后对若干传统经验的修正。文中明确区分已实测事实与待验证推论。

---

## 一、架构前提：为什么 OpenRR 的内核套路不能直接做成通用“光追遍历插件”

AMD 光追并非“纯靠硬件、软件无能为力”——恰恰相反，AMD 的 Ray Accelerator（RA）只提供单步求交指令（RDNA2/3 为 BVH4，RDNA4 升级为 BVH8 + 双三角形求交 + OBB），整个 BVH 遍历循环、短栈管理、BVH 构建与 SBT 着色器派发全部运行在 CU 着色器上（Windows 由闭源 GPURT + LLPC 生成，Linux 由 Mesa RADV + ACO 生成）。

但 OpenRR 在光线重建上取得大幅提速的三个前提，**游戏光追本身几乎均不满足**：

| 维度 | OpenRR（光线重建 / 降噪） | 游戏光追本身（BVH 构建 + 遍历 + 着色） |
| :--- | :--- | :--- |
| **替换边界** | 干净的 NGX / Streamline 接口，输入 G-Buffer、输出重建图像；固定约百个已知 compute kernel 即可整体替换 | 与引擎深度耦合：自定义顶点/索引布局、全局 Bindless 材质表、SBT、Payload、AnyHit 透贴、BRDF、ReSTIR 蓄水池，无通用替换边界 |
| **低精度红利** | 密集矩阵乘与卷积，RDNA4 可用 WMMA `FP8×FP8→FP32` 翻倍吞吐并减半权重/特征带宽 | 瓶颈在**指针追踪随机访存延迟 + 波前分支发散 + VGPR 压力**；射线求交对精度高度敏感，无法套用 FP8 算力红利 |
| **绕过闭源驱动的成本** | Root Signature / Pipeline Layout 由 OpenRR 自控，可离线用 Mesa ACO 产出 `.nrp` 直接注入 `CreateComputePipelineFromElf` 或 `VK_KHR_pipeline_binary` | 游戏拥有成百上千个动态着色器，且依赖驱动构建的私有 BLAS/TLAS 布局与引擎自身的全局描述符堆 |

### 借道 `Mesa ACO → PAL ELF / Pipeline Binary` 改游戏光追的三道硬门槛
1. **`DispatchRays`（RT 管线）vs `RayQuery`（内联光追）**：
   - `DispatchRays`（`ID3D12StateObject` / `VkPipeline` RT）含多阶段拼接、relocation、continuation stack 与 GPURT 私有调用约定，`CreateComputePipelineFromElf` 无法直接接管。
   - `RayQuery` Compute Shader 本质是标准 Compute PSO，二进制注入通道在机制上可行，但仍受下面两道门槛制约。
2. **BVH 显存布局对齐（Windows GPURT vs Mesa RADV）**：
   - 硬件求交指令消费的节点格式固定，但加速结构 Header、实例描述符扩展字段、元数据与压缩标记在 GPURT 与 RADV 间并不天然字节一致，需专门对齐 traversal lowering。
3. **Root Signature 与全局 Bindless Descriptor ABI**：
   - OpenRR 靠自定 ABI（`openrr-d3d12-v1`）规避了描述符差异；替换游戏自身着色器则必须让 NIR/ACO 直接按 Windows 原生 D3D12 Root Table / 全局 Bindless Heap 偏移生成访存指令。

---

## 二、真机实测校准：RX 9070 XT（RDNA4）路径追踪 7 轮单变量对比

为检验 `TraceRay (RGS)`、`RayQuery (RGS)` 与 `RayQuery (CS)` 在 RDNA4 Windows 驱动下的真实表现，在固定存档、固定站位与光照条件下对 RTX Remix（`dxvk-remix`，`remix-main+a00da12a`）进行了全屏 4K 性能档（`1920×1080 → 3840×2160`）实测：

- **测试硬件与栈**：AMD Radeon RX 9070 XT（`gfx1201`），Windows Vulkan 驱动 `26.11.2`；OpenRR `v0.1.7plus` 原生挂载游戏 `VkDevice`（ACO strict，fast mode）。
- **关键对照组数据**：
  - **`(2, 1, 2)`（GBuffer `TraceRay RGS` + Direct `RayQuery RGS` + Indirect `TraceRay RGS`）**：稳态 **29.6–30.0 FPS**，热缓存复测高峰 **30.4 FPS**（最稳最快）；
  - **`(1, 1, 2)`（GBuffer `RayQuery RGS` + Direct `RayQuery RGS` + Indirect `TraceRay RGS`）**：稳态 **~29.6–29.9 FPS**；
  - **`(0, 1, 2)`（仅 Direct 切 `RayQuery RGS`）**：稳态 **29.6–29.8 FPS**（单点贡献 +1.1～1.2 FPS，且消除掉帧抖动）；
  - **`(2, 0, 2)`（仅 GBuffer 切 `TraceRay RGS`，Direct 留 `CS`）**：稳态 **29.0–29.4 FPS**（偶发跌至 28.7 FPS，帧时间抖动）；
  - **`(0, 0, 2)`（RTX Remix 官方 AMD `Auto` 默认）**：稳态 **28.4–28.7 FPS**；
  - **`(2, 1, 1)`（Indirect 切 `RayQuery RGS`）**：稳态 **25.6–25.8 FPS**（每帧慢约 5.5 ms）；
  - **`(0, 0, 0)`（全 `RayQuery CS`）**：稳态 **~15 FPS**（性能直接腰斩）。
- **OpenRR 自身表现与帧时间拆解**：上述全部轮次中，OpenRR 在 `1920×1080 → 3840×2160` 下的 GPU 耗时始终稳定在 **`p50 7.40–7.45 ms`**（显存约 886 MB），开启 OpenRR 后的总帧率与“关闭 RR 改走引擎多 Pass NRD 降噪 + FSR 4.1 桥接”完全持平（均在 ~29–30 FPS）。以最优 `(2, 1, 2)` 档位 `~30 FPS`（总帧时间 `~33.3 ms`）为基线，OpenRR 固定约占 **22%**（`~7.4 ms`），其余 **~78%（`~25.9 ms`）均为非 OpenRR 的路径追踪与渲染主干开销**——这也解释了为何直接削减光线追踪主干预算对总帧率的收益远大于继续压榨后处理或单核指令。

---

## 三、实测对传统理论猜测的 3 项关键修正

### 1. 证伪“把 `DispatchRays` 改成 `RayQuery (CS)` 普遍能提升 AMD 帧率”的笼统经验
- **旧经验来源**：RDNA2 早期及单跳简单阴影/RTAO 场景中，内联 `RayQuery (CS)` 常因省去 SBT 跳转而更快（这也是 `dxvk-remix` 将 AMD 默认值硬编码为 `(0, 0, 2)` 的历史原因）。
- **已实测事实（RDNA4 重度路径追踪拆解出两组正交对比）**：
  - **同在 `RGS` 壳子内，阶段隔离显著胜过单一内联内核**：多反弹间接光（Indirect）下，`TraceRay (RGS)`（`(2, 1, 2)`，`29.6–30.0 FPS`）显著快于 `RayQuery (RGS)`（`(2, 1, 1)`，`25.6–25.8 FPS`，每帧慢约 `5.5 ms`）。这说明在多材质与多反弹复杂度下，RDNA4 的 RT 管线阶段拆分与寄存器生命周期隔离已大幅优于强行内联进单一巨型着色器。
  - **同样使用内联 `RayQuery` 逻辑，`RGS` 管线壳子显著胜过普通 `CS` 壳子**：
    - Direct（带半透明阴影与重采样）：同在内联求交下，走 `RGS` 壳子（`(2, 1, 2)` `29.6–30.0 FPS` / `(0, 1, 2)` `29.6–29.8 FPS`）稳定高于走普通 `CS` 壳子（`(2, 0, 2)` `29.0–29.4 FPS` 且偶发跌至 `28.7 FPS` 抖动；`(0, 0, 2)` `28.4–28.7 FPS`）；
    - Indirect（多反弹）：`RayQuery (RGS)`（`(2, 1, 1)`）为 `25.6–25.8 FPS`，而全落入普通 `RayQuery (CS)`（`(0, 0, 0)`）则暴跌至 `~15 FPS`（单帧再慢约 `27 ms`，帧率直接腰斩）。
- **高概率原因（待验证推论）**：Windows AMD Vulkan 驱动通常将普通 Compute Shader 默认按 **Wave64** 调度，发散光线在 64 宽波前下的长尾等待与 VGPR 压力显著高于 `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`；此外，`RGS` 与 `CS` 两类管线在 RDNA4 **Dynamic VGPR** 分配策略、遍历短栈（short stack）放置位置及 LLPC 针对 RT 管线的专属优化通道上也可能存在差异。
  - **低成本验证路径**：可通过 `VK_KHR_pipeline_executable_properties` 或 Radeon GPU Profiler（RGP）直接核对 Direct/Indirect `CS` 与 `RGS` 的实际 `subgroupSize`、VGPR 占用与 scratch bytes，或通过单变量为 Direct/Indirect `CS` 挂接 `requiredSubgroupSize = 32` 实测波前宽度本身带来的改善幅度。
- **工程准则**：切勿在重度发散光追场景中盲目把 `DispatchRays` 往普通 `RayQuery (CS)` 改；只有极简、单跳、无复杂材质分支的高相干查询才适合普通 `CS`。

### 2. 比“硬啃 ACO 重编游戏光追内核”投入产出比高得多的两个切入点
实测与日志分析表明，在触碰底层 ISA 重编之前，存在两座收益更高、成本低一个数量级的优化空间：

1. **解除生态中的“Warp32 假设软门禁”（倍级收益潜力）**
   - 许多偏 NVIDIA 生态开发的光追特性默认假设设备 `subgroupSize == 32`。例如 RTX Remix 的稀疏光追前置核 `compact_active_pixels.comp.slang` 检测到 AMD 默认汇报 `subgroupSize = 64` 便直接关闭整个 Sparse Rendering，迫使 GPU 追踪 100% 像素（而非设定的 10%～30% 活跃像素）。
   - RDNA2/3/4 实际原生支持 `VK_EXT_subgroup_size_control`（`minSubgroupSize = 32, maxSubgroupSize = 64`）。通过为特定压缩核指定 `requiredSubgroupSize = 32`（或改写 64 宽波前归约）解锁稀疏光追，并由 `7.4 ms` 的 OpenRR 负责稀疏重建，属于**直接削减 70%～90% 光线预算**的算法级提速，收益远超指令级调优。
   - **落地前置约束与待验项**：
     - **32-bit Subgroup 安全边界**：切勿只在设备属性层全局伪装 `VkPhysicalDeviceSubgroupProperties::subgroupSize = 32` 而不锁定实际管线波前；必须在管线创建时为 `compact_active_pixels.comp.slang`（并审计所有依赖 32-bit `subgroupBallot().x` 位掩码的关联着色器）显式挂接 `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo{ requiredSubgroupSize = 32 }`（同时注意与 `ALLOW_VARYING_SUBGROUP_SIZE` / `REQUIRE_FULL_SUBGROUPS` 标志的合规组合），否则驱动按 Wave64 编译后会静默丢失或写坏高 32 lane。
     - **NGX / OpenRR 输入契约与阿姆达尔定律预期**：开启 `rtx.sparseRendering` 后需实机核验 `dxvk-remix` 传入 DLSS-RR（`nvngx_dlssd.dll` 310.9.1）的输入缓冲约定（未追踪像素以 `0`、NaN 还是历史重投影填充，以及是否附带额外引导图/标志），确保 OpenRR 重建无黑点或残影；同时在 `~7.4 ms` OpenRR 及固定渲染开销的阿姆达尔定律约束下，削减 70%～90% 光线预算对应的整帧预期增益约为 **~1.6×–2.3×**（已属显著跃升，而非整帧直接翻 3×–10×）。
2. **修正发散型 `RayQuery (CS)` 的默认 Wave64 错配（低成本 5%～15% 级潜力，待跨游戏进一步验证）**
   - 对使用 `RayQuery (CS)` 的 Vulkan / DX12 光追游戏，无需先处理 GPURT 布局或离线 ACO 注入；可优先评估通过 Vulkan Layer 挂接 `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo{ requiredSubgroupSize = 32 }`（或 DX12 `[WaveSize(32)]`）将高发散光追 CS 切回 Wave32，测试能否直接消除类似 Direct CS 的波前拖尾与抖动。

### 3. 收窄“单游戏离线 ACO 替换 `RayQuery CS`”的适用边界
- `Indirect CS` 跌至 15 FPS 的实测现象侧面印证了：巨型 `RayQuery` uber-shader 在 Windows LLPC 下确实会发生严重的寄存器溢出。
- 但同一组测试也表明：**只要引擎具备 `TraceRay (RGS)` 路径（30 FPS），架构级拆分就远胜于在单核 CS 上换编译器修补**（即使 ACO 为 15 FPS 的 CS 追回 25%，也仅约 19 FPS）。
- 因此，借鉴 OpenRR 的 `ACO → PAL ELF / Pipeline Binary` 去做游戏光追外科手术，**只应在以下条件同时满足时才值得立项**：
  1. 目标为值得单独维护的热门大作，且引擎写死了巨型 `RayQuery (CS)`、不存在可切换的 RGS 路径；
  2. 先尝试低成本强制 Wave32 后仍未解决瓶颈；
  3. 抓取管线统计确认该 `RayQuery (CS)` 在 Windows LLPC 下确实存在严重 VGPR spill 或明显冗余计算（同时可结合着色器逻辑精简与半精度材质计算，由 OpenRR 兜底降噪）。

---

## 四、跨游戏可扩展性分级与总体优先级

### 跨游戏可扩展性
- **全套 RTX Remix 游戏（直接复用）**：共用 `dxvk-remix` 后端，`(2, 1, 2)` 全 RGS 调优及后续稀疏光追 Wave32 修复可直接复用于同系全部游戏。
- **其他重度路径光追商业引擎（规律复用）**：无现成 ini 切换开关，但“多反弹/半透明高发散光追忌讳落入 Wave64 单核 CS”与“优先保降采样/缓存特性开启”的诊断逻辑通用。
- **轻度混合光追游戏（不可生搬）**：单跳简单阴影/AO 着色器极小，不适用“RGS 必然快于 CS”的推论，需实测判断。

### OpenRR 视角下的光追体验优化优先级
1. **第一优先级（OpenRR 主业 + 生态门禁解锁）**：持续压低 OpenRR 自身帧时与显存开销，并在支持的引擎/Mod 中解除因 `subgroupSize = 64` 或厂商检测被误关的稀疏光追、光线缓存与激进降采样选项——**用 `~7 ms` 的高质量重建换取数倍光线削减**。
2. **第二优先级（管线模式与 Wave32 调度校准）**：在提供管线开关的引擎中避开高发散光追走默认 Wave64 CS（如 RTX Remix 切 `(2, 1, 2)`）；对纯 `RayQuery (CS)` 游戏优先探索低侵入的 `Wave32` 强制调度。
3. **第三优先级（单游戏离线 ACO 手术）**：仅作为前两步均无法覆盖、且实锤 LLPC 严重 spill 时的最后定制手段，不追求做成通用光追遍历替换插件。
