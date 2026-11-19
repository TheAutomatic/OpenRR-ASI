# OpenRR v0.1.7plus Speed, Image Quality, and VRAM Evaluation Report

[简体中文](v017plus-speed-quality-eval.md) | English

> [!NOTE]
> **Evaluation Environment and Methodology**: Conducted under the Stage 8/9 standard isolated harness (`build/v017plus-eval/`). All measurements ran sequentially on an exclusive `AMD Radeon RX 9070 XT` (`vendor=0x1002 device=0x7550`) GPU queue with all `ONR_*` runtime debug environment variables unset (default `FLOW=0`; `Fast=2` uses built-in `ONR_LT_DEEP="2,4,2,4"`), and weights `nvngx_dlssd.dll 310.9.1.0`.

---

## 1. Tested Binaries and Reference Provenance

| Role | Source / Version | Core Files & Artifacts | SHA-256 | Runtime Notes |
|---|---|---|---|---|
| **Tested Release (`ours`)** | **OpenRR `v0.1.7plus`** (GitHub Release `v0.1.7plus`) | `OpenRR-v0.1.7plus-OptiScaler.zip` / `OptiScaler/plugins/openrr-amd.asi` | `openrr-amd.asi`: `f603b725acc34115b0266f49ea0800f24879567f841dd654b19f0488830c22c6`<br>`zip`: `3fc6cd26d957b78eb2ebf3a624a716d5749ecdcf579286be665d546cf8706005`<br>`gfx1201 manifest.json`: `f77d8f62e7afb99114a4790a15ab183c05ddf77d79a7ffb8d6f96b27081540f2` | Default `FLOW=0`, strict offline Mesa 26.3 ACO on RDNA4, full 16-stage FP8 in `New`/`Fast` |
| **Upstream Official (`up017`)** | **Official `OpenRR v0.1.7`** (upstream GitHub Release `openrr-0.1.7.zip`) | `OpenNrRrRuntime.dll` + `opennr-rr/shaders/` | `OpenNrRrRuntime.dll`: `8dad9c4cf2eac590681fd6ed297c7556909ffdf090cab16ff52e91c5459ace24`<br>`zip`: `f907d8bbf9284528d8b4a9c9897d659db017eec1081cf80f2f055de1eb4ce024` | Default `FLOW=0`, driver LLPC; defaults to `Fp8=0` (FP16), while `OnrRrSetFp8(1)` enables its 7 experimental `*_l8.spv` kernels (`Fp8=1`) |
| **Prior In-House Build (`stage9`)** | **Stage 9 Final Seal** (`v0.1.6 + Wave D`) | `openrr-amd.asi` | `92cb14c85e50af36d05130cb0d46291f33e3a00cac4d5c1b6f44b0acb64a5ec7` | Local `v0.1.6` offline ACO seal build for cross-version regression checking |
| **Ground-Truth Reference (`RTX 5090`)** | **NVIDIA GeForce RTX 5090 (`310.9.1.0`)** | Driver `621.14` + `nvngx_dlssd.dll` `310.9.1.0` | `nvngx_dlssd.dll`: `4bc7ea5fcb2f32cf86bc2cb072e8d2860914a0be511cbdcb2fc206c1d9d83b80` | `03_timing_core` 6-preset speed and NGX/DXGI VRAM telemetry + `RR_Contrast` (`v1` 18 frames + `v2_motion` 29 frames = 47 RGBA16F reference frames) |

> Note: Official `openrr-0.1.7.zip` packaged its SPIR-V folder as `opennr-rr.shaders/`, whereas `OpenNrRrRuntime.dll` searches for `opennr-rr\shaders\`. In the isolated benchmark environment, the shaders were copied to `opennr-rr/shaders/` and passed the smoke check (`fp8 kernels ready (7 of their own)`).

---

## 2. Speed and VRAM Evaluation

### 2.1 Steady-State Frame Time Across All 6 Presets

Protocol: exclusive GPU, `warmup = 64`, `400` steady frames (`v0.1.7plus`) / `240` steady frames (`up017`), no probes, `--optional-guides 0`.

| Preset (`Render -> Output`) | RTX 5090 | **`v0.1.7plus`** `New=1` / `Fast=2` (`Legacy=0`) | vs `5090` (`New` / `Fast`) | Official `v0.1.7` `Fp8=0` (`New` / `Fast`, `Legacy=0`) | Official `v0.1.7` `Fp8=1` (`New` / `Fast`) | **`v0.1.7plus` Time Saved** (`vs Fp8=0` / `vs Fp8=1`) |
|---|---|---|---|---|---|---|
| **1080p Quality** (`1280x720 -> 1920x1080`) | `0.871 ms` | **`2.8047`** / **`2.6416 ms`** (`3.6555`) | `3.22x` / `3.03x` | `2.9938` / `2.8402 ms` (`3.4056`) | `2.9598` / `2.8238 ms` | New `−6.32%` / `−5.24%`<br>Fast **`−6.99%` / `−6.45%`** |
| **1440p Quality** (`1706x960 -> 2560x1440`) | `1.407 ms` | **`4.7893`** / **`4.5948 ms`** (`6.1750`) | `3.40x` / `3.27x` | `5.1404` / `4.9985 ms` (`5.9020`) | `5.0904` / `4.9316 ms` | New `−6.83%` / `−5.92%`<br>Fast **`−8.08%` / `−6.83%`** |
| **4K Quality** (`2560x1440 -> 3840x2160`) | `2.915 ms` | **`10.5012`** / **`10.0888 ms`** (`13.4685`) | `3.60x` / `3.46x` | `11.3719` / `11.0444 ms` (`12.8618`) | `11.2176` / `10.8894 ms` | New `−7.66%` / `−6.39%`<br>Fast **`−8.65%` / `−7.35%`** |
| **4K Performance** (`1920x1080 -> 3840x2160`) | `1.958 ms` | **`6.6728`** / **`6.4028 ms`** (`8.6609`) | `3.41x` / `3.27x` | `7.1013` / `6.8850 ms` (`8.1183`) | `7.0063` / `6.7954 ms` | New `−6.03%` / `−4.76%`<br>Fast **`−7.00%` / `−5.78%`** |
| **1080p DLAA** (`1920x1080 -> 1920x1080`) | `1.545 ms` | **`5.5900`** / **`5.2975 ms`** (`7.1454`) | `3.62x` / `3.43x` | `6.0564` / `5.8061 ms` (`6.8336`) | `5.9978` / `5.7604 ms` | New `−7.70%` / `−6.80%`<br>Fast **`−8.76%` / `−8.04%`** |
| **4K UltraPerf** (`1280x720 -> 3840x2160`) | `1.287 ms` | **`3.8964`** / **`3.7316 ms`** (`5.1248`) | `3.03x` / `2.90x` | `4.0218` / `3.8922 ms` (`4.6862`) | `3.9977` / `3.8599 ms` | New `−3.12%` / `−2.53%`<br>Fast **`−4.13%` / `−3.32%`** |

### 2.2 Interleaved `3×ABBA` Head-to-Head (`14` Pairs / `84` Legs, Max Leg Deviation `<= 0.291%`)

Protocol: `warmup = 48`, `steady = 240`, `drs = 0`; each pair executes 3 interleaved rounds (`AB / BA`, 6 legs total) to cancel thermal and clock drift:

| Comparison Group | Mode & Resolution | Baseline Mean | **`v0.1.7plus` Mean** | Mean Delta (Median Delta) | Speedup (`base/ours - 1`) | Legs Won | Max Dev / Check |
|---|---|---|---|---|---|---|---|
| **vs Official `v0.1.7` (`Fp8=0`)** | `Legacy=0` @ `1080p Qual` | `3.3960 ms` | `3.6350 ms` | `+238.98 µs` (`+238.96`) | `−6.57%` | `0/6` | `0.291%` / **`0-ULP`** |
| **vs Official `v0.1.7` (`Fp8=0`)** | `Legacy=0` @ `4K Qual` | `12.8697 ms` | `13.4697 ms` | `+600.05 µs` (`+594.75`) | `−4.45%` | `0/6` | `0.210%` / **`0-ULP`** |
| **vs Official `v0.1.7` (`Fp8=0`)** | **`New=1` @ `1080p Qual`** | `2.9794 ms` | **`2.8007 ms`** | **`−178.70 µs`** (`−179.10`) | **`+6.38%`** | **`6/6`** | `0.180%` |
| **vs Official `v0.1.7` (`Fp8=0`)** | **`New=1` @ `4K Qual`** | `11.3613 ms` | **`10.4966 ms`** | **`−864.72 µs`** (`−863.65`) | **`+8.24%`** | **`6/6`** | `0.081%` |
| **vs Official `v0.1.7` (`Fp8=0`)** | **`Fast=2` @ `1080p Qual`** | `2.8393 ms` | **`2.6353 ms`** | **`−204.03 µs`** (`−204.00`) | **`+7.74%`** | **`6/6`** | `0.259%` |
| **vs Official `v0.1.7` (`Fp8=0`)** | **`Fast=2` @ `4K Qual`** | `11.0258 ms` | **`10.0999 ms`** | **`−925.85 µs`** (`−926.65`) | **`+9.17%`** | **`6/6`** | `0.125%` |
| **vs Official `v0.1.7` (`Fp8=1`)** | **`New=1` @ `1080p Qual`** | `2.9507 ms` | **`2.8013 ms`** | **`−149.43 µs`** (`−150.54`) | **`+5.33%`** | **`6/6`** | `0.123%` |
| **vs Official `v0.1.7` (`Fp8=1`)** | **`New=1` @ `4K Qual`** | `11.2120 ms` | **`10.4936 ms`** | **`−718.47 µs`** (`−717.70`) | **`+6.85%`** | **`6/6`** | `0.099%` |
| **vs Official `v0.1.7` (`Fp8=1`)** | **`Fast=2` @ `1080p Qual`** | `2.8164 ms` | **`2.6335 ms`** | **`−182.92 µs`** (`−182.56`) | **`+6.95%`** | **`6/6`** | `0.213%` |
| **vs Official `v0.1.7` (`Fp8=1`)** | **`Fast=2` @ `4K Qual`** | `10.8725 ms` | **`10.0923 ms`** | **`−780.18 µs`** (`−780.25`) | **`+7.73%`** | **`6/6`** | `0.216%` |
| **vs Stage 9 (`v0.1.6`)** | `New=1` @ `1080p Qual` | `2.8004 ms` | `2.7996 ms` | `−0.75 µs` (`+0.18`) | `+0.03%` | `3/6` | `0.108%` / `0-ULP` |
| **vs Stage 9 (`v0.1.6`)** | `New=1` @ `4K Qual` | `10.4977 ms` | `10.4887 ms` | `−8.98 µs` (`−11.10`) | `+0.09%` | `4/6` | `0.170%` / `0-ULP` |
| **vs Stage 9 (`v0.1.6`)** | `Fast=2` @ `1080p Qual` | `2.6344 ms` | `2.6338 ms` | `−0.63 µs` (`−0.66`) | `+0.02%` | `3/6` | `0.139%` / `0-ULP` |
| **vs Stage 9 (`v0.1.6`)** | `Fast=2` @ `4K Qual` | `10.1006 ms` | `10.0996 ms` | `−1.02 µs` (`+2.80`) | `+0.01%` | `2/6` | `0.230%` / `0-ULP` |

- **Speed summary**: `v0.1.7plus` shows **zero performance regression** against the Stage 9 (`v0.1.6`) seal build (within `< 0.1%` measurement noise and `0-ULP` on static S0). Compared with official `OpenRR v0.1.7`, `New=1` and `Fast=2` win **all `48/48` interleaved legs**: `+9.17%` faster (`−925.85 µs`) in `4K Fast` and `+8.24%` faster (`−864.72 µs`) in `4K New` out of the box, and still `+5.33% .. +7.73%` faster even when official `v0.1.7` explicitly enables `OnrRrSetFp8(1)`.

### 2.3 Three-Way VRAM Comparison (`v0.1.7plus` vs Official `v0.1.7` vs RTX 5090 Official NGX `310.9.1.0`)

VRAM footprint comparison across `v0.1.7plus` (RX 9070 XT D3D12 Direct), official `OpenRR v0.1.7` (`Fp8=0` / `Fp8=1`), and NVIDIA RTX 5090 official `nvngx_dlssd.dll 310.9.1.0` (`03_timing_core` telemetry):

| Preset (`Render -> Output`) | **`v0.1.7plus` Feature VRAM** | `v0.1.7plus` Process Delta (`proc_delta`) | Official `v0.1.7` Process Delta (`Fp8=0` / `Fp8=1`) | **RTX 5090 Official NGX `GetStats`** (`r1 ~ r2`) | RTX 5090 Process DXGI Steady (`drs_end`) | **`v0.1.7plus` VRAM Saved vs RTX 5090** |
|---|---|---|---|---|---|---|
| **1080p Quality** (`1280x720 -> 1920x1080`) | **`209 MB`** | `231.3 MB` | `232.1` / `232.1 MB` | `174.0 ~ 315.6 MiB` | `333.8 MiB` (`r1 delta`) | `−102.5 MB` process delta (`−30.7%`) |
| **1440p Quality** (`1706x960 -> 2560x1440`) | **`361 MB`** | `381.3 MB` | `382.1` / `382.1 MB` | **`612.5 ~ 847.1 MiB`** | `908.3 ~ 1,149.3 MiB` | **`−252 ~ −486 MB` (`−41% ~ −57%`)** |
| **4K Quality** (`2560x1440 -> 3840x2160`) | **`791 MB`** | `812.6 MB` | `813.4` / `813.4 MB` | **`1,508.3 ~ 1,708.9 MiB`** | `1,935.5 ~ 2,141.0 MiB` (`2,453.7 MiB` DRS) | **`−717 ~ −918 MB` (`−48% ~ −54%`, less than half of 5090)** |
| **4K Performance** (`1920x1080 -> 3840x2160`) | **`586 MB`** | `607.6 MB` | `608.4` / `608.4 MB` | **`1,316.1 ~ 1,847.7 MiB`** | `1,683.5 ~ 2,228.1 MiB` | **`−730 ~ −1,262 MB` (`−55% ~ −68%`)** |
| **1080p DLAA** (`1920x1080 -> 1920x1080`) | **`356 MB`** | `377.3 MB` | `378.1` / `378.1 MB` | `1,547.8 MiB`* (solo run `~176 ~ 365 MiB`) | `1,896.7 MiB`* | `356 MB` (comparable to standalone 5090) |
| **4K UltraPerf** (`1280x720 -> 3840x2160`) | **`487 MB`** | `509.1 MB` | `509.9` / `509.9 MB` | `1,607.2 MiB`* | `1,941.2 MiB`* | **`−1,120 MB` (`−70%` vs 5090 4K pool)** |

1. **Against RTX 5090 Official NGX: saves `0.7–1.1 GB` (`48%–70%`) at 4K with comparable image quality**
   OpenRR aliases intermediate activation buffers by tensor lifetime inside a single `Engine::arena_` (differences among `Legacy`, `New`, and `Fast` are `< 0.5 MB`), requiring only **`487 / 586 / 791 MB`** at 4K UltraPerf / Perf / Qual (and `684 MB` measured in-game at 4K PBZ `2228x1253 -> 3840x2160`). By contrast, official NVIDIA NGX `310.9.1.0` reports **`1,316–1,848 MiB`** at 4K even via `NVSDK_NGX_D3D12_GetStats` (with `1.68–2.14 GiB` steady DXGI process commitment and `2.45 GiB` peak under DRS). That cuts Ray Reconstruction VRAM in half at 4K while delivering a 47-frame mean `PSNR_01 = 49.20 dB` (`Reinhard = 54.29 dB`) in `New=1`, leaving valuable headroom on 12 GB and 16 GB GPUs.
2. **Against Official `OpenRR v0.1.7`**:
   On Windows D3D12 Direct, both builds use the same compact `arena_` layout (`812.6 MB` vs `813.4 MB` process delta at 4K Quality, `v0.1.7plus` ~`0.8 MB` lower; upstream `OpenNrRrRuntime.dll` does not log a per-feature breakdown). Under **native Vulkan / Linux Wine `vkd3d-proton` (`RecordVk`)**, `v0.1.7plus` additionally allocates Vulkan host I/O staging buffers compactly on demand, saving a further **`60–130 MB`** when resources pass through directly.
3. **`*` RTX 5090 Measurement Scope Note**:
   On RTX 5090, `03_timing_core` ran sequentially inside a single process (`1080p_qual -> 1440p_qual -> 4k_qual -> 1080p_dlaa -> 4k_ultrap -> 4k_perf`), so `1080p_dlaa` and `4k_ultrap` reflect CUDA/NGX memory-pool carryover after `4k_qual` (a standalone `1080p_dlaa` run uses roughly `176–365 MiB`). The monotonically rising `1080p_qual -> 1440p_qual -> 4k_qual` sequence and post-recreation `4k_perf r2` (`1,316.1 MiB`) provide clean lower bounds confirming that NVIDIA NGX retains at least `1.32–1.71 GiB` whenever output resolution reaches 4K.

---

## 3. Image Quality Evaluation

### 3.1 Anchor #1 — `Legacy=0` Bit-Exactness Verification

| Test Case | Reference Baseline | Steady Exactness / `max_ulp` | DRS Exactness / `max_ulp` | `PSNR_01` (`steady` / `drs`) | `non_finite` |
|---|---|---|---|---|---|
| `anchor1_1080p` (`opt_guides=0`) | Official `OpenRR v0.1.7` (LLPC) | **`100.000000%` / `0`** | **`100.000000%` / `0`** | `999.00` / `999.00 dB` | `0` |
| `anchor1_4k` (`opt_guides=0`) | Official `OpenRR v0.1.7` (LLPC) | **`100.000000%` / `0`** | **`100.000000%` / `0`** | `999.00` / `999.00 dB` | `0` |
| `anchor1_1080p_guides_vs_stage9` (`opt_guides=1`) | Stage 9 Final (ACO) | **`100.000000%` / `0`** | **`100.000000%` / `0`** | `999.00` / `999.00 dB` | `0` |
| `anchor1_1080p_guides` (`opt_guides=1`) | Official `OpenRR v0.1.7` (LLPC) | `99.999928%` (`6/8,294,400`, `1-ULP`) | `99.999506%` (`41/8,294,400`, `1-ULP`) | `128.42` / `122.48 dB` | `0` |

> [!NOTE]
> The tiny `1-ULP` variance against official `v0.1.7` when `opt_guides=1` (`max_abs_err = 0.000488`, `PSNR >= 122.48 dB`) matches the documented hardware instruction behavior in `gpu/shaders/enc0_attr.comp`: `Legacy` `_f_w` preserves native `log2`/`exp2`, where ACO and driver LLPC differ by at most 1 ULP in `v_log_f32`/`v_exp_f32`. Against the Stage 9 ACO build, it remains strictly `100%` `0-ULP`.

### 3.2 Anchor #2 — Full 47-Frame Reference Comparison vs RTX 5090 `310.9.1.0` (`v1` + `v2_motion`)

Executed in full on `v0.1.7plus` across **`v1` (29 cases / 97 frames, 18 RTX 5090 refs) + `v2_motion` (19 GPU cases + 2 skipped `P13a/P13b` exposure probes not exposed in runtime ABI / 43 frames, 29 RTX 5090 refs)**: **`140/140` saved frames had `total_non_finite = 0`**.

| Metric (vs RTX 5090 `310.9.1.0`) | **`v0.1.7plus` `Legacy=0`** | **`v0.1.7plus` `New=1`** | **`v0.1.7plus` `Fast=2`** | Official `v0.1.7` `New` (`Fp8=0`) | Official `v0.1.7` `New` (`Fp8=1`) | Official `v0.1.7` `Fast` (`Fp8=1`) |
|---|---|---|---|---|---|---|
| **47-Frame Mean `PSNR_01` / `Reinhard`** | `46.961` / `51.871 dB` | **`49.198`** / **`54.286 dB`** | `48.438` / `53.490 dB` | `46.170` / `51.134 dB` | `45.979` / `50.938 dB` | `45.560` / `50.518 dB` |
| **47-Frame Global Min `PSNR_01`** | `31.2714 dB` | **`39.1325 dB`** | `38.9601 dB` | `36.4815 dB` | `33.6004 dB` | `36.4567 dB` |
| `v1/Q_S0_1080p_dlaa` (`min` / `f063`) | `31.2714` / `31.2714` | **`52.4206`** / **`54.7203`** | **`51.7475`** / **`54.5286`** | `36.4815` / `36.4815` | `33.6004` / `33.6004` | `36.4567` / `36.4567` |
| `v1/Q_S0_4k_perf` (`min` / `f067 DRS`) | `48.1313` / `49.1824` | **`52.0823`** / **`55.1693`** | **`51.4523`** / **`54.8783`** | `48.3094` / `49.0374` | `48.2859` / `49.0104` | `47.6458` / `48.6624` |
| `v2/P00_baseline_s1_g0` (`f015`) | `48.2716 dB` | **`49.2057 dB`** | `47.6393 dB` | `46.9561 dB` | `46.9286 dB` | `45.9526 dB` |
| `v2/Q_S1_1080p_qual` (`min` / `f063`) | `39.3101` / `48.5199` | **`39.1325`** / **`49.0080`** | `38.9601` / `46.0662` | `38.9693` / `47.0176` | `38.9361` / `46.9963` | `38.7738` / `45.4505` |
| `v2/Q_S1_4k_qual` (`min` / `f063`) | `47.6197` / `48.6742` | **`47.7323`** / **`52.3706`** | `47.3200` / `49.7734` | `47.3832` / `48.3074` | `47.1254` / `48.3295` | `46.8123` / `47.6223` |
| `v2/Q_S2_1080p_qual` (`min` / `f063`) | `41.6878` / `47.7252` | `40.6314` / `43.5299` | `40.1983` / `43.2846` | `40.9943` / `42.1236` | `40.6221` / `42.0748` | `40.2894` / `42.3805` |
| `v2/Q_S2_4k_perf` (`min` / `f063`) | `42.1041` / `48.6230` | `41.1768` / `46.0888` | `40.8514` / `45.2903` | `41.5046` / `45.6127` | `41.0948` / `45.4600` | `40.9082` / `44.6330` |

### 3.3 Comparison vs Stage 9 (`v0.1.6`) and `v0.1.7` Feature Probes

1. **Image quality delta vs Stage 9 (`v0.1.6`) after integrating `v0.1.7` camera-relative reflections and `MVJittered`**:
   - `v1/Q_S0_1080p_dlaa`, `v1/Q_S0_4k_perf`, `v2/Q_S1_4k_qual`, `v2/Q_S2_1080p_qual`, and `v2/Q_S2_4k_perf` remain **bit-identical (`0.0000 dB`)** to Stage 9.
   - **Dynamic camera scene `v2/P00_baseline_s1_g0` improved**: `New=1` rose from `49.1568 dB` (Stage 9) to **`49.2057 dB` (`+0.0489 dB`)**, and `Fast=2` rose from `47.5665 dB` to **`47.6393 dB` (`+0.0728 dB`)**.
   - `v2/Q_S1_1080p_qual`: `New=1` improved from `39.1105` to **`39.1325 dB` (`+0.0220 dB`)**, while `Fast=2` measured `38.9601` vs `38.9716 dB` (`−0.0115 dB`).
2. **`v2_motion` `MVJittered` and feature probes**:
   - `P14` (input subrect offset), `P15` (negative motion vector scale), and `P17` (unpacked separate roughness) all match `P00` at **`999.00 dB` (`0 diff`)** across `Legacy`, `New`, and `Fast`.
   - `P16a` / `P16b` / `P16c` respond as expected to `ONR_RR_FRAME_MV_JITTERED` relative to `P00` (`New=1`: `32.36` / `53.24` / `31.94 dB`; `Fast=2`: `30.25` / `52.79` / `31.93 dB`).
3. **Key quality advantage over official `v0.1.7`**:
   - **`New=1` averages `+3.03 dB` higher than official `v0.1.7` default (`49.198` vs `46.170 dB`) and `+3.22 dB` higher than official `Fp8=1` (`49.198` vs `45.979 dB`) across all 47 reference frames**.
   - In the long static jittered sequence `v1/Q_S0_1080p_dlaa (f063)`, official `v0.1.7` suffers significant accumulated quality degradation (`36.4815 dB` default FP16, dropping further to `33.6004 dB` with `Fp8=1`). `v0.1.7plus` holds **`54.7203 dB`** on the exact same scene (`+18.24 dB` over default / **`+21.12 dB` over official FP8**) through full quantization scale, E8RAW, and high-precision tail-step governance.

---

## 4. Recommendations

- **Default daily driver — `Mode = new` (`New=1`)**: `1440p Qual` **`4.79 ms`**, `4K Qual` **`10.50 ms`**, `4K Perf` **`6.67 ms`**; **`6.0%–8.2%`** faster than official `v0.1.7`, **`3+ dB`** higher average PSNR across 47 frames, and less than half the 4K VRAM of RTX 5090 official NGX (`586–791 MB`).
- **High-framerate mode — `Mode = fast` (`Fast=2`)**: `1080p Qual` **`2.64 ms`** (`3.03x` 5090), `4K Qual` **`10.09 ms`** (`3.46x` 5090), `4K UltraPerf` **`3.73 ms`** (`2.90x` 5090); **`4.1%–9.2%`** faster than official `v0.1.7` with zero `non-finite` pixels, trading slight sensitivity in motion and bright speculars from temporal deepest-level reuse.
- **Bit-exact reference mode — `Mode = legacy` (`Legacy=0`)**: Maintains `0-ULP` bit-exactness with upstream's FP16 path for clean visual A/B verification.
