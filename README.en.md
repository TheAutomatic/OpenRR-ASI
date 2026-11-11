# OpenRR-ASI (`openrr-amd.asi`)

[简体中文](README.md) | English

This repository is a **standalone OptiScaler `.asi` plugin fork** based on [OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR). It runs NVIDIA DLSS Ray Reconstruction (`nvngx_dlssd.dll` 310.9.1) on AMD Radeon RX 9000 series (RDNA4) GPUs.

For details about the OpenRR network reimplementation, RDNA4 shader architecture, clean-room validation methodology, and benchmarks, see the upstream **[OpenNeuralRendering/OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** repository.

---

## What this fork adds

### Optional OptiFG host mode

The general release configuration keeps `[OpenRR] OptiFGHost=false` in `openrr-amd.ini`, using the standalone RR path by default. For **frame generation with upscaler input** (for example, `FGInput=upscaler` and `FGOutput=xefg`), follow the [OptiFG installation guide](asi/OPTIFG.md) (in Chinese) to configure the additional `openrr-ngx-host.dll`, then set `OptiFGHost=true` and restart the game. Do not use `_nvngx.dll` from older test packages.

Once host mode is enabled, turning off FG in the menu does not automatically leave host mode. To return to standalone RR, set the option back to `false` and restart. Host wrapping and parameter handling may add overhead, while FG has its own resource preparation and interpolation costs. The performance difference relative to standalone RR has not yet been quantified. Updating only the ASI while leaving this option disabled does not activate the host bridge.

Upstream OpenRR builds as an external `OpenNrRrRuntime.dll` by default and requires a specially modified OptiScaler build to call it. This fork packages it as a single `openrr-amd.asi` plugin for **official, unmodified [OptiScaler](https://github.com/optiscaler/OptiScaler)**, with the following features and fixes.

### 1. Single-file `.asi` plugin and a MinGW build toolchain

- **No separate runtime DLL**: `OpenNrRrRuntime` and the GPU engine are statically embedded in `openrr-amd.asi` (approximately 3.0 MB after stripping a release build). Place it in the official OptiScaler `plugins/` directory to handle DLSS Ray Reconstruction (DLSS-D).
- **Minimal dependencies and cross-platform support**: the MinGW-w64 build script (`asi/build/build_asi.sh`) statically links runtime libraries and resolves Vulkan entry points on demand through assembly stubs. Dependencies are limited to basic Windows system DLLs, with support for native Windows and Linux Wine / Proton (RADV).

### 2. NGX interception, state restoration, and multiple backends

- **Lightweight capability detection**: rewrites NGX `SuperSamplingDenoising` capability parameters. The first query performs a lightweight synchronous check of the `nvngx_dlssd.dll` 310.9.1 version and Vulkan RDNA4 features at instance level, with a 30-second cache for failed checks. Weight parsing and pipeline compilation run on a background thread after `CreateFeature` to avoid blocking the game main thread.
- **Complete compute state restoration for D3D12 Direct**: uses MinHook to intercept underlying `D3D12Core` functions as soon as a D3D12 device is discovered, without modifying object vtables. Tracks and restores descriptor heaps, the compute root signature, all root arguments (descriptor tables, CBVs, SRVs, UAVs, and 32-bit root constants), and the PSO / state object after `Record`. This supports games such as Resident Evil 9 (`re9.exe`), Monster Hunter Wilds, Dragon's Dogma 2, and 007 First Light without relying on state restoration inside a custom OptiScaler build.
- **Automatic D3D12 ↔ Vulkan fallback and bridge hardening**: when direct ELF execution through `amdxc64.dll` is unavailable (for example, Linux vkd3d-proton), or when `Backend = vulkan` is selected, uses command-list splitting and Vulkan shared-resource interoperability. Includes protection against recursive fallback and rejection rules for split barriers, enhanced barriers, RTAS, and meta commands.
- **Native Vulkan games (`RecordVk`)**: watches for late loading of `vulkan-1.dll` through `LdrRegisterDllNotification`. Injects the required extensions, features, and a separate compute queue during the game's `vkCreateDevice`, and hides RR when the device has not been prepared.
- **Streamline RR without global DXGI spoofing**: when used with `OptiPatcher.asi` (`Dxgi=auto/false`), OpenRR temporarily reports supported AMD adapters as NVIDIA in `sl.common` SystemCaps only during the synchronous `slOnPluginLoad` call in `sl.dlss_d.dll`. It restores each modified field on return and removes two NVX-only Vulkan extensions from the plugin's returned JSON. Known limitation: this assumes no other thread reads these capabilities while `slInit` is loading plugins.

### 3. Coexistence with [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD)

- When `dlssnr-amd.asi` and `openrr-amd.asi` are both in `plugins/`, no configuration changes are required for either plugin. OpenRR automatically selects D3D12 Direct without adding another command-list vtable hook, recognizes and tracks continuation command lists after splitting, and passes through the `Insert` (OptiScaler) and `End` (`dlssnr-amd`) hotkeys while the menu is open.

### 4. Built-in ImGui sidebar

- Follows `OptiScaler.ini [Menu] ShortcutKey` (`Insert` by default) and docks to the right of the main OptiScaler panel, respecting its menu scale. Supports SDR, HDR10 PQ, and scRGB composition.
- Allows in-game switching of `Fast` / `Legacy` network modes, the D3D12 backend, depth type, and additional guides. Changes are saved automatically to `openrr-amd.ini`.

### 5. Fixes in `gpu/` and `runtime/`

These fixes have separate commits to make upstream integration easier.

- **Verified GPU retirement and an overflow slot pool**: replaces reclamation based on recording counts or timeouts with GPU timestamp writeback combined with game-side `ID3D12CommandAllocator::Reset` / `VkCommandPool` reset evidence. Uses 64 primary slots (with `kOwnSlot` reserved for internally owned lists) and 64 overflow slots allocated as needed. When all slots are occupied, safely skips a frame instead of throwing.
- **GPU buffer fills during recording and batched initialization**: fixes D3D12 `fill` operations exceeding the 64 KB upload ring while bound, and the two-argument Vulkan `fill` resetting the game's `VkCommandBuffer` while bound. Uses GPU-side `clear_range.comp` dispatches and `vkCmdFillBuffer`, batching initialization into a single submission during `Engine::resize`.
- **Two GPU timing metrics, on-demand VRAM allocation, and history reset**: reports raw GPU time (`raw` last / p50 / p95) alongside an estimated `clean` time that filters cross-queue asynchronous compute interference. Vulkan host input/output buffers use compact on-demand allocation, saving 60–130 MB when all resources can pass through directly. Returning after a pause or game menu lasting more than 250 ms sets `ONR_RR_FRAME_RESET` to avoid stale-history ghosting.

---

## Requirements

- **GPU and driver**: AMD Radeon RX 9000 series (RDNA4), with support for `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8` (`shaderFloat8` and `shaderFloat8CooperativeMatrix`), and `VK_KHR_workgroup_memory_explicit_layout`. Windows D3D12 Direct also requires `IAmdExtD3DDevice5` from `amdxc64.dll`.
- **Vulkan**: Vulkan 1.3 with `hostQueryReset` and `scalarBlockLayout`.
- **Model weights**: `nvngx_dlssd.dll` version **310.9.1** is required.

---

## Installation and usage

### Option 1: Installer (recommended)

Use the **[dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)** launcher for automatic deployment, configuration management, and installation alongside [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD).

### Option 2: Manual installation with official OptiScaler

Place the build artifacts in the game directory containing OptiScaler's `dxgi.dll` / `OptiScaler.ini`:

| File / directory | Destination relative to `OptiScaler.ini` |
|---|---|
| `openrr-amd.asi` | `plugins/openrr-amd.asi` |
| `openrr-amd.ini` | `openrr-amd.ini` |
| `openrr-amd/shaders/*.spv` | `openrr-amd/shaders/*.spv` (90 `.spv` files) |
| `nvngx_dlssd.dll` (**310.9.1**) | Game directory or `openrr-amd/nvngx_dlssd.dll` |

Enable ASI plugin loading in `OptiScaler.ini` (disabled by default in OptiScaler):

```ini
[Plugins]
LoadAsiPlugins=true
```

> **Important:**
> 1. **Do not rename the plugin to `*-loadlate.asi`.** OptiScaler loads `loadlate` plugins on a separate thread after `LateAsiPluginsDelay` (30 seconds by default). By then, the game has already queried NGX capabilities and called `vkCreateDevice`, so OpenRR cannot update RR capabilities or prepare the Vulkan device.
> 2. **Coexistence with [`dlssnr-amd.asi`](https://gitlab.com/mochizuki0323/DLSSNR-AMD):** place both plugins directly in `plugins/`, without `loadlate` in either name. OptiScaler loads plugins alphabetically, so OpenRR installs its hooks after `dlssnr-amd` and becomes the outermost NGX hook, taking priority for RR calls. No changes to the `dlssnr-amd` configuration are needed.

---

## Building (WSL / Linux MinGW-w64)

Install `x86_64-w64-mingw32-g++`, `python3`, and `glslangValidator`, then run:

```bash
bash asi/build/build_asi.sh
```

The distributable package is generated in `build/asi/stage/`. Separate debug symbols are saved to `build/asi/openrr-amd.debug`, and the unit tests and command-list splitting validation suite are built as `build/asi/test_asi_units.exe`.

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

MIT License. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).
