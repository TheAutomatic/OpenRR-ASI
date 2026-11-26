# OpenRR-ASI (`openrr-amd.asi`)

[简体中文](README.md) | English

> [!IMPORTANT]
> **Source-Available · Completely Free · No Resale or Stripped Attribution**: Original works, modifications, the standalone ASI plugin, packaging tools, and compiled release binaries by `TheAutomatic` in this repository are licensed under **[PolyForm Noncommercial License 1.0.0](LICENSE)** for personal, noncommercial use only—commercial resale, paid bundles/groups/downloads, and paid installation services are strictly prohibited. **Noncommercial forks and derivative works must comply with [NOTICE](NOTICE)**: you may credit your own modifications, but MUST preserve attribution for the original author (`TheAutomatic`), all upstream authors (including [OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR) and [mochizuki0323/DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)), the official repository URL, and all `Required Notice:` lines; stripping attribution or altering binary metadata/version numbers to claim original authorship is prohibited. If you paid for this tool, request a refund immediately! See [License](#license).

This repository is a **standalone OptiScaler `.asi` plugin fork** based on [OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR). It runs NVIDIA DLSS Ray Reconstruction (`nvngx_dlssd.dll` 310.9.1) on AMD Radeon RX 9000 series (RDNA4) and, experimentally, RX 7000 / RDNA3 / RDNA3.5 GPUs.

For details about the OpenRR network reimplementation, RDNA4 shader architecture, clean-room validation methodology, and benchmarks, see the upstream **[OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** repository.

---

This release (**v0.1.7plus7**) refactors OpenRR-ASI into a native **OptiScaler v0.10 nightly (Plugin API v1–v3)** plugin while keeping all fixes from **v0.1.7plus6** (Linux Wine / Proton + `vkd3d-proton` direct Vulkan interop), **v0.1.7plus5** (`NvAPI_GPU_GetGPUInfo` `0xafd1b02c` fallback compatibility and parallelized build/packaging), and **v0.1.7plus4** / **v0.1.7plus3** (ReShade + RenoDX D3D12 wrapper compatibility and D3D12 hook hardening):
- **Native `OptiProvider_Dlssd` registration**: registers directly as a first-class OptiScaler Ray Reconstruction provider via Plugin API v3 `RegisterProvider(..., "OpenRR", OptiProvider_Dlssd, ...)`, removing ~30 outer `NVSDK_NGX_*` MinHooks, `SlCapsPatch`, and the separate `openrr-ngx-host.dll` bridge. Downstream OptiScaler features (`OptiFG`, `XeFG`, `HUDfix`, `OutputScaling`, `RCAS`, and live RR upscaler switching) now work natively without extra host DLL configuration or restarts.
- **Native `PLUGINS -> OpenRR` tab inside OptiScaler**: removes the standalone DX11/DX12/Vulkan ImGui overlay, SwapChain / `Present` hooks, WndProc / User32 input interception, and bundled UI font. Settings now live directly in OptiScaler's `PLUGINS -> OpenRR` tab with a one-click `Re-init Upscaler` action and zero-hook CJK font configuration (`OptiScaler.ini [Menu] UseHQFont=true` + `TTFFontPath`, safely falling back to English until configured and restarted).
- **Requirement**: requires **OptiScaler v0.10 nightly** (with Plugin API v3 `RegisterProvider`). Legacy `openrr-ngx-host.dll`, `OptiFGHost`, `MenuKey`, `MenuScale`, `TextScale`, and `Theme` options have been removed; clean up any old `openrr-ngx-host.dll` and replace `openrr-amd.ini` when upgrading.

It continues to integrate upstream **v0.1.7** (including fixed RDNA4 ACO routing, RX 7000 / RDNA3 `shaders-gfx11` support, and a unified Windows + Linux Wine / Proton Mesa RADV >= 26.3 release), while retaining GPU resource lifetime protections and performance/VRAM optimizations. See **[v017plus-speed-quality-eval.en.md](docs/test-evidence/v017plus-speed-quality-eval.en.md)** ([简体中文](docs/test-evidence/v017plus-speed-quality-eval.md)) for the full v0.1.7plus speed, image quality, and VRAM evaluation, and [v017-stage10-audit.md](docs/test-evidence/v017-stage10-audit.md) for Stage 10 validation evidence; historical figures for earlier versions are retained at the bottom.

## What this fork adds

Upstream OpenRR builds as an external `OpenNrRrRuntime.dll` by default and requires a specially modified OptiScaler build to call it. This fork packages it as a single native `openrr-amd.asi` plugin for **official, unmodified [OptiScaler](https://github.com/optiscaler/OptiScaler) v0.10 nightly**, with the following features and fixes.

### 1. Single-file native `.asi` plugin and a MinGW build toolchain

- **No separate runtime DLL**: `OpenNrRrRuntime` and the GPU engine are statically embedded in `openrr-amd.asi` (even smaller and leaner after removing the standalone ImGui overlay and DX11/DX12/Vulkan overlay renderers). Place it in OptiScaler's `OptiScaler/plugins/` directory to register `"OpenRR"` as a native DLSSD provider and settings tab via `OptiPluginInit`.
- **Minimal dependencies and cross-platform support**: the MinGW-w64 build script (`asi/build/build_asi.sh`) statically links runtime libraries and resolves Vulkan entry points on demand through assembly stubs. Dependencies are limited to basic Windows system DLLs, with support for native Windows and Linux Wine / Proton (RADV).

#### Network modes, performance, and VRAM overview (RX 9070 XT, `v0.1.7plus`)

Switch live at runtime from OptiScaler's `PLUGINS -> OpenRR` tab or via `[OpenRR] Mode` (`new` / `fast` / `legacy`) in `openrr-amd.ini` (on first launch, `New` and `Fast` pipelines compile in the background while output falls back to `Legacy` until ready):

| Mode | 1080p Quality | 1440p Quality | 4K Quality | Speedup vs Official `v0.1.7` (`3×ABBA`) | Quality vs RTX 5090 (47-Frame Mean `PSNR_01` / Reinhard) |
|---|---|---|---|---|---|
| **`New` (default)** | `2.80 ms` | `4.79 ms` | `10.50 ms` | **`+6.4% – +8.2%`** (still `+5.3% – +6.9%` faster than `Fp8=1`) | **`49.20 dB` / `54.29 dB`** (`+3.0 dB` above official `v0.1.7`) |
| **`Fast` (experimental)** | `2.64 ms` | `4.59 ms` | `10.09 ms` | **`+7.7% – +9.2%`** (`0.93 ms` saved at 4K) | `48.44 dB` / `53.49 dB` (deepest temporal reuse; slightly more sensitive on motion/bright highlights) |
| **`Legacy` (reference)** | `3.66 ms` | `6.18 ms` | `13.47 ms` | **`0-ULP` bit-exact** with upstream FP16 path | `46.96 dB` / `51.87 dB` |

- **Less than half the VRAM of RTX 5090**: lifetime-aliased `Engine::arena_` allocation uses only **`209 / 361 / 586 / 791 MB`** of feature VRAM at 1080p Qual / 1440p Qual / 4K Perf / 4K Qual—saving **`0.7–1.1 GB` (`48%–70%`) at 4K** compared with official NVIDIA NGX on RTX 5090 (`1.32–1.71 GiB` resident, `2.45 GiB` peak under DRS) at comparable image quality. Native Vulkan (`RecordVk`) direct pass-through saves an extra `60–130 MB` over upstream.
- See the full 6-preset benchmarks, 84-leg `3×ABBA` data, three-way VRAM breakdown, and `v1 + v2_motion` image quality results in **[v0.1.7plus Speed, Image Quality, and VRAM Evaluation Report](docs/test-evidence/v017plus-speed-quality-eval.en.md)** ([简体中文](docs/test-evidence/v017plus-speed-quality-eval.md)).

### 2. Native OptiScaler DLSSD provider, state restoration, and multiple backends

- **Native `OptiProvider_Dlssd` integration**: registers `"OpenRR"` directly with OptiScaler Plugin API v3 instead of hooking external `NVSDK_NGX_*` exports. OptiScaler handles upstream input routing, Streamline `sl.dlss_d` spoofing, and downstream `OptiFG` / `XeFG` / `HUDfix` / `OutputScaling` / `RCAS`; switch providers live in OptiScaler's `Upscalers -> RR Upscaler` menu (`auto` selects OpenRR automatically on AMD GPUs).
- **Lightweight capability detection**: the first capability query performs a lightweight synchronous check of `nvngx_dlssd.dll` 310.9.1 and instance-level Vulkan features with a 30-second cache for failed checks. Weight parsing and pipeline compilation run on a background thread after `CreateFeature` to avoid blocking the game main thread.
- **Non-intrusive compute state restoration for D3D12 Direct**: never calls `SetDescriptorHeaps` or touches graphics root state during recording (descriptors are read directly via raw GPU addresses in root constants). Uses MinHook to intercept underlying `D3D12Core` functions as soon as a D3D12 device is discovered, without modifying object vtables—observing host heap switches read-only to invalidate stale table handles, and restoring the compute root signature, all compute root arguments (descriptor tables, CBVs, SRVs, UAVs, and 32-bit root constants), and the PSO / state object after `Record`. Supports games such as Resident Evil 9 (`re9.exe`) and 007 First Light natively.
- **Automatic D3D12 ↔ Vulkan fallback and bridge hardening**: when direct ELF execution through `amdxc64.dll` is unavailable (for example, Linux vkd3d-proton), or when `Backend = vulkan` is selected, uses command-list splitting and Vulkan shared-resource interoperability. Includes protection against recursive fallback and rejection rules for split barriers, enhanced barriers, RTAS, and meta commands.
- **Native Vulkan games (`RecordVk`)**: watches for late loading of `vulkan-1.dll` through `LdrRegisterDllNotification`. Injects the required extensions, features, and a separate compute queue during the game's `vkCreateDevice`, and refuses feature creation cleanly if the device was not prepared.

### 3. Coexistence with [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD)

- When `dlssnr-amd.asi` and `openrr-amd.asi` are both in `OptiScaler/plugins/`, no configuration changes are required for either plugin. OpenRR automatically detects coexistence, selects D3D12 Direct without adding another command-list vtable hook, and recognizes continuation command lists after splitting.

### 4. Native OptiScaler `PLUGINS -> OpenRR` tab

- Registered via `OptiPluginInit` + `RegisterTab`, inheriting OptiScaler's menu shortcut (`Insert` by default), theme, HDR rendering, and scaling without any separate SwapChain or input hooks.
- Displays live status and dual-track GPU timings, switches `New` / `Fast` / `Legacy` network modes, D3D12 backend, depth type, and guides, and provides a `Re-init Upscaler` button (`ReInitUpscalers`) so backend or depth changes take effect without restarting. Settings stay in `openrr-amd.ini`.
- **Zero-hook CJK localization**: leverages OptiScaler nightly's dynamic font backend. Selecting `zh-CN`, `ja`, or `ko` offers a one-click `Configure CJK Font in OptiScaler.ini` button that sets `[Menu] UseHQFont=true` and `TTFFontPath` (preferring bundled `openrr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf`, then Windows system fonts); until configured and restarted, the tab safely stays in English so missing-glyph boxes never appear.

### 5. Fixes in `gpu/` and `runtime/`

These fixes have separate commits to make upstream integration easier.

- **Verified GPU retirement and an overflow slot pool**: replaces reclamation based on recording counts or timeouts with GPU timestamp writeback combined with game-side `ID3D12CommandAllocator::Reset` / `VkCommandPool` reset evidence. Uses 64 primary slots (with `kOwnSlot` reserved for internally owned lists) and 64 overflow slots allocated as needed. When all slots are occupied, safely skips a frame instead of throwing.
- **GPU buffer fills during recording and batched initialization**: fixes D3D12 `fill` operations exceeding the 64 KB upload ring while bound, and the two-argument Vulkan `fill` resetting the game's `VkCommandBuffer` while bound. Uses GPU-side `clear_range.comp` dispatches and `vkCmdFillBuffer`, batching initialization into a single submission during `Engine::resize`.
- **Two GPU timing metrics, on-demand VRAM allocation, and history reset**: reports raw GPU time (`raw` last / p50 / p95) alongside an estimated `clean` time that filters cross-queue asynchronous compute interference. Vulkan host input/output buffers use compact on-demand allocation, saving 60–130 MB when all resources can pass through directly. Returning after a pause or game menu lasting more than 250 ms sets `ONR_RR_FRAME_RESET` to avoid stale-history ghosting.

---

## Requirements

- **OptiScaler**: requires **OptiScaler v0.10 nightly** (with Plugin API v3 `RegisterProvider` and `RegisterTab`).
- **GPU and driver**:
  - **Windows RDNA4 (RX 9000 series, `gfx1201`/`gfx1200`)**: fixed strict **ACO** offline pipeline route; Vulkan requires `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8` (`shaderFloat8` and `shaderFloat8CooperativeMatrix`), `VK_KHR_workgroup_memory_explicit_layout`, `VK_KHR_pipeline_binary`, and `maintenance5`. Windows D3D12 Direct also requires `IAmdExtD3DDevice5` from `amdxc64.dll`.
  - **Linux Wine / Proton (Mesa RADV >= 26.3)**: uses system Mesa RADV directly (no custom `libvulkan_radeon.so` or wrapper script needed); RDNA4 runs native RADV ACO with in-memory `SpvDecorationNoContraction` hardening on `shaders/` via `winevulkan.dll` and `vkd3d-proton` `RecordVk` interop.
  - **RDNA3 / RDNA3.5 (RX 7000 series / 780M, `gfx11`)**: fixed separate `shaders-gfx11/` FP16 / LLPC path.
- **Vulkan**: Vulkan 1.3 with `hostQueryReset` and `scalarBlockLayout`.
- **Model weights**: `nvngx_dlssd.dll` version **310.9.1** is required.

---

## Installation and usage

### Option 1: Installer (recommended)

Use the **[dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)** launcher for automatic deployment, configuration management, and installation alongside [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD).

### Option 2: Manual installation with official OptiScaler v0.10 nightly (unified Windows / Linux package)

> When upgrading from `<= v0.1.7plus6`, remove any leftover `openrr-ngx-host.dll` from the game directory, clear any manual `[Libraries] NvngxPath` override in `OptiScaler.ini`, and replace `openrr-amd.ini` with the new file from the archive.

Place the build artifacts (the 392-file unified `stage/` package, or extract `dist/OpenRR-v0.1.7plus7-OptiScaler.zip`) directly into the game directory containing OptiScaler's `dxgi.dll` / `OptiScaler.ini`:

| File / directory | Destination relative to `OptiScaler.ini` |
|---|---|
| `openrr-amd.asi` | `OptiScaler/plugins/openrr-amd.asi` |
| `openrr-amd.ini` | `openrr-amd.ini` |
| `openrr-amd/shaders/*.spv` | `openrr-amd/shaders/*.spv` (`94` `.spv` files) |
| `openrr-amd/shaders-gfx11/*.spv` | `openrr-amd/shaders-gfx11/*.spv` (`94` `.spv` files, required for RDNA3 / RDNA3.5) |
| `openrr-amd/aco/gfx1201/openrr-d3d12-v1/`, `openrr-amd/aco/gfx1200/openrr-d3d12-v1/` | Keep directory structure; `94` `.nrp`, `manifest.json`, and `manifest.msgpack` per target (required on Windows RDNA4, ignored automatically on Linux Mesa RADV) |
| `openrr-amd/lang/`, `openrr-amd/OpenRR_*.{txt,md}` | `openrr-amd/lang/` (translations & `fonts/`) plus `OpenRR_LICENSE.txt`, `OpenRR_NOTICE.txt`, and `OpenRR_THIRD_PARTY.md` inside `openrr-amd/` |
| `nvngx_dlssd.dll` (**310.9.1**) | Game directory or `openrr-amd/nvngx_dlssd.dll` |

Enable ASI plugin loading in `OptiScaler.ini` (disabled by default in OptiScaler):

```ini
[Plugins]
LoadAsiPlugins=true
```

#### Linux (Wine / Proton + system Mesa RADV >= 26.3) notes

- The exact same unified release package runs on Linux Wine / Proton as long as **system Mesa RADV >= 26.3** is installed — no custom `libvulkan_radeon.so`, `with_mesa263.sh` launcher, or `VK_ICD_FILENAMES` override is required.
- OpenRR resolves Vulkan via `winevulkan.dll`, uses native `vkd3d-proton` `RecordVk` interop, applies in-memory `SpvDecorationNoContraction` hardening to `shaders/*.spv`, and skips Windows PAL `aco/*.nrp` binaries automatically.
- Alongside OptiScaler v0.10 nightly (`dxgi.dll`, `OptiScaler.ini` with `LoadAsiPlugins=true`) and `nvngx_dlssd.dll` (**310.9.1**), copy **`OptiScaler/plugins/openrr-amd.asi`** (keep exact `OptiScaler` casing on case-sensitive Linux filesystems so extracting merges into OptiScaler v0.10's `OptiScaler/plugins/` directory), `openrr-amd/` (`shaders/`, `shaders-gfx11/`, `lang/`), and `openrr-amd.ini` into the game directory (not a standalone `.asi`; extracting the release zip handles all paths automatically), and set Steam launch options to `WINEDLLOVERRIDES="dxgi=n,b" %command%`.

> **Important:**
> 1. **Do not rename the plugin to `*-loadlate.asi`.** OptiScaler loads `loadlate` plugins on a separate thread after `LateAsiPluginsDelay` (30 seconds by default). By then, the game has already queried NGX capabilities and called `vkCreateDevice`, so OpenRR cannot register its DLSSD provider or prepare the Vulkan device.
> 2. **Coexistence with [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD):** place both plugins directly in `OptiScaler/plugins/`, without `loadlate` in either name. OpenRR registers through OptiScaler's native provider API and automatically selects D3D12 Direct mode; no changes to the `dlssnr-amd` configuration are needed.

---

## Building and Packaging (WSL / Linux / GitHub Actions)

Install `x86_64-w64-mingw32-g++`, `python3`, and `glslangValidator`, then run:

```bash
bash asi/build/build_asi.sh
```

The staged package is generated in `build/asi/stage/`. Separate debug symbols are saved to `build/asi/openrr-amd.debug`, and the unit tests and command-list splitting validation suite are built as `build/asi/test_asi_units.exe`. When both offline ACO targets (`gfx1201`/`gfx1200`) are present or `OPENRR_PACKAGE_ZIP=1` is set, the build validates the stage tree and emits a deterministic release archive at `dist/OpenRR-v<version>-OptiScaler.zip` (plus `.sha256`); you can also run `python asi/build/package_release.py` (or `asi/build/PACKAGE_LOCAL_RELEASE.cmd` on Windows) directly. GitHub Actions packaging is defined in [`.github/workflows/package.yml`](.github/workflows/package.yml).

---

## Test and validation matrix

Hardware: **AMD Radeon RX 9070 XT (RDNA4, gfx1201)**, driver **26.10.1 (LLPC)**, and `nvngx_dlssd.dll` **310.9.1.0**. See the [hardware validation and benchmark log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) for full evidence.

| Test / scenario | Path / backend | Result | Evidence |
|---|---|---|---|
| Copied command-list vtable recursion protection and `CreateCommandList` fallback (#11) | D3D12 Split / Hook | **PASS**: no recursion or `E_FAIL` | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| Split barrier / enhanced barrier / RTAS / meta command / bundle rejection (#12) | D3D12 Split `prepare()` | **PASS**: all five unsupported cases rejected; recovery after `END_ONLY` | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| Hotkey parsing: single digit `1`, decimal `45`, `0x2D`, and key combinations (#13) | `openrr::pe::parse_key` | **PASS** | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| Bound and unbound 256 KB `D3dDevice::fill`, exceeding the 64 KB upload ring (#2) | D3D12 Direct `clear_range` | **PASS**: `unbound wrong=0, bound wrong=0` | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 127 occupied slots, `ONR_RR_BUSY`, and reclamation after `CommandAllocator::Reset` (#3/#4) | `OnrRrRecord` D3D12 Direct | **PASS**: first 127 slots return `ONR_RR_OK`; slot 128 returns `ONR_RR_BUSY (-5)`; immediately returns `ONR_RR_OK` after reset | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| D3D12 ↔ Vulkan shared-resource interoperability (#7) | `RecordInputs` / `Enqueue` / `RecordOutput` | **PASS**: `backend=0 (VULKAN)`; all three stages return `ONR_RR_OK (0)` | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |
| 1080p / 4K steady state, Fast/Legacy A/B, DRS, K2 hash checks, and B1/B2 profiling (K1/K2/#9/#10) | D3D12 Direct Engine | **PASS**: three same-process A/B rounds against `d6f70fb`. 1080p Fast median: 3.124 vs 3.171 ms (−1.5%), 251 MB VRAM; 1440p: 5.391 vs 5.451 ms (−1.1%), 435 MB; 4K: 11.812 vs 11.941 ms (−1.1%), 960 MB. B1 separate pooling, `wide` path, and `f0` race control: `max_ulp_diff=0`. B2 Fast lossy approximation: `PSNR_01=55.5–60.4 dB`, `PSNR_tonemap=59.9–65.2 dB`. K2: `spv_hash_nopad=EXACT`. | [Log](docs/test-evidence/rx9070xt-d3d12-validation-2026-11-08.log) |

## License

### Free Software, Anti-Resale & Derivative Attribution Notice
This software is distributed **free of charge** for personal, noncommercial use. **Selling this plugin or its release packages, bundling it in paid modpacks or paywalled groups/downloads, or charging for "remote installation / setup / tuning" services is strictly prohibited.** If you paid for this package, request a refund immediately!

**Noncommercial forks, modifications, repackaged builds, and derivative works MUST comply with [NOTICE](NOTICE)**: derivative authors may credit their own changes, but MUST keep original author attribution (`TheAutomatic`), all upstream copyright notices, the official repository URL, and all `Required Notice:` lines intact. **Stripping attribution or altering binary metadata/version numbers to claim original authorship is strictly prohibited.**

### License Scope & Upstream Attributions
- **[This Repository (TheAutomatic / OpenRR-ASI)](https://github.com/TheAutomatic/OpenRR-ASI)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)** (see [NOTICE](NOTICE)): Original additions, modifications, standalone ASI plugin, build/packaging tooling, and compiled release binaries (`openrr-amd.asi`, precompiled shader/ACO records).
- **[Upstream OpenRR (OpenNeuralRendering/OpenRR)](https://github.com/OpenNeuralRendering/OpenRR)** — **MIT License** (Copyright (c) 2026 OpenNR): Core GPU kernels, engine, runtime, and validators (full upstream MIT text preserved in [THIRD_PARTY.md](THIRD_PARTY.md)).
- **Other Upstream & Third-Party Components** (`mochizuki0323/DLSSNR-AMD`, OptiScaler Plugin SDK, MinHook, Vulkan-Headers, Mesa RADV/ACO, Noto Sans CJK SC, etc.) retain their respective open-source licenses; see [NOTICE](NOTICE) and [THIRD_PARTY.md](THIRD_PARTY.md).

> This repository and its release packages contain **no** proprietary NVIDIA binaries (such as `nvngx_dlssd.dll`), extracted neural network weights, or OptiScaler binaries. Users supply their own `nvngx_dlssd.dll` (`310.9.1`).
