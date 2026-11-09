# OpenRR-ASI (`openrr-amd.asi`) 实机游戏初始基线验证报告 (2026-11-09)

本报告记录 `openrr-amd.asi` (`v0.1.2`, 提交 `aaee7cc` / `59049e9`) 作为独立 OptiScaler 插件在三款 D3D12 光追/重度游戏中的实机运行基线、显存占用、GPU 帧耗时及与 `dlssnr-amd.asi` (`0.0.8.4-t14`) 共存表现。

- **测试硬件**：AMD Radeon RX 9070 XT (`gfx1201` / RDNA4)，Adrenalin 26.10.1 (`32.0.32015.7029`, LLPC)
- **测试环境**：Windows 11 (`10.0.26300`)，开启 OptiScaler DXGI + Vulkan 显卡伪装（伪装显示为 `NVIDIA GeForce RTX 4090`），同时加载 `openrr-amd.asi` 与 `dlssnr-amd.asi`
- **原始日志归档位置（本地保留，`.gitignore` 忽略不入库）**：`docs/test-evidence/game-logs/2026-11-09-initial-baseline/`

---

## 1. 三款实机游戏运行总览

| 游戏名称 | 插件版本 | 渲染 -> 输出分辨率 (档位) | 执行后端与网络模式 | 接入的引导图 (Guides) | OpenRR 自身显存 | DLSSNR 自身显存 | 游戏进程总显存 / 预算 | OpenRR GPU Clean 耗时 (p50 / p95) | 累计运行帧数与退出状态 |
| :--- | :--- | :--- | :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| **赛博朋克 2077**<br>(`Cyberpunk2077.exe`) | `0.1.2 (aaee7cc)` | `1920x1080 -> 3840x2160`<br>(4K Performance, 网格 `1920x1088`) | D3D12 Direct<br>Fast (`_lt`) + `coexist_mochi=yes` | Color, Diffuse/Specular Albedo, Normals+Roughness, Depth, MV, Specular HitDist, **Transparency Layer**, **SSS Guide** | **732 MB** | **282.7 MB**<br>(1080p Before: 池 167.0 + 权重 115.7) | `11,932 / 13,490 MB`<br>(88.5%) | **7.75 ~ 8.05 ms**<br>(`p50 7.75~8.08`, `p95 7.87~8.18`) | **3,600+ 帧**<br>0 跳帧 / 0 错误 |
| **影之刃零**<br>(`PBZ.exe`, 长测) | `0.1.2 (aaee7cc)` | `2562x1441 -> 3840x2160`<br>(4K Quality, 网格 `2592x1472`) | D3D12 Direct<br>Fast (`_lt`) + `coexist_mochi=yes` | Color, Diffuse/Specular Albedo, Normals+Roughness, Depth, MV, Specular HitDist, **Disocclusion Mask (0..1)**, **SSS Guide** | **981 MB** | **645.0 MB**<br>(4K Present: 池 529.2 + 权重 115.8) | `6,224 / 15,419 MB`<br>(40.4%) | **11.95 ~ 12.10 ms**<br>(`p50 12.03~14.56`, `p95 12.33~19.99`) | **2,892 帧**<br>正常 `Destroy` 释放 |
| **影之刃零**<br>(`PBZ.exe`, 缓存复测) | `0.1.2 (59049e9)` | `2562x1441 -> 3840x2160`<br>(4K Quality, 网格 `2592x1472`) | D3D12 Direct<br>Fast (`_lt`) + `coexist_mochi=yes` | 同上 | **981 MB** | **645.0 MB** | `7,391 / 15,419 MB`<br>(47.9%) | **~11.95 ms**<br>(缓存秒开 `0.50s + 0.47s`) | **192 帧**<br>正常 `Destroy` 释放 |
| **红色沙漠**<br>(`CrimsonDesert.exe`) | `0.1.2 (59049e9)` | `1920x1080 -> 3840x2160`<br>(4K Performance, 网格 `1920x1088`) | D3D12 Direct<br>Fast (`_lt`) + `coexist_mochi=yes` | Color, Diffuse/Specular Albedo, Normals+Roughness, Depth, MV, Specular HitDist, **Disocclusion Mask (0..1)**, **SSS Guide** | **732 MB** | **645.0 MB**<br>(4K Present: 中途热切换开关验证) | `4,830 / 15,419 MB`<br>(31.3%) | **7.24 ~ 7.67 ms**<br>(`p50 7.22~7.66`, `p95 7.47~8.94`) | **8,400+ 帧**<br>正常退出 (`0x0`) |

---

## 2. 显存占用专项基线对比

### 2.1 不同输入分辨率下的 OpenRR 自身显存占用

| 输出分辨率 | 渲染输入分辨率 (档位) | 内部对齐网格 (`grid`) | 内部超分尺度 (`internal`) | OpenRR 自身显存 (`this feature`) | 说明 |
| :---: | :---: | :---: | :---: | :---: | :--- |
| `3840x2160` | `1920x1080` (4K Performance) | `1920x1088` | `5760x3240` | **732 MB** | 在《赛博朋克 2077》与《红色沙漠》中完全一致（精确到 MB） |
| `3840x2160` | `2562x1441` (4K Quality) | `2592x1472` | `5760x3240` | **981 MB** | 在《影之刃零 PBZ》中稳定为 `981 MB`；高输入网格的编码/解码激活池与历史缓冲增量为 `+249 MB` |

### 2.2 《影之刃零 PBZ》历史版本显存演进对比 (`2562x1441 -> 3840x2160`)

| Runtime / ASI 版本 | 网络模式 | OpenRR 自身显存 | 稳态 GPU 耗时 (avg) | 关键变化说明 |
| :--- | :--- | :---: | :---: | :--- |
| `OpenRR 0.1.0` (`opennr_rr.log` 历史记录) | Fast (`_f` + 早期池) | `1,272 MB` | `15.51 ~ 15.97 ms` | 未合入激活池紧凑复用与 `_lt` 快速核 |
| `OpenRR 0.1.1` (`opennr_rr.log` 历史记录) | Fast (`_lt` B0/B1/B2) | `983 MB` | `11.80 ~ 12.05 ms` | 激活池复用节省 **289 MB**，`_lt` 核提速 **~3.7 ms** |
| **`openrr-amd.asi 0.1.2`** (`aaee7cc` / `59049e9`) | **Fast (`_lt` B0/B1/B2)** | **`981 MB`** | **`11.95 ~ 12.10 ms`** | 独立 ASI 插件化 + R1–R10 生命周期加固后，显存与性能保持最优基线 |

---

## 3. 分阶段帧耗时明细

### 3.1 《赛博朋克 2077》 (`1920x1080 -> 3840x2160`, D3D12 Direct Fast)

| 采样帧号 | GPU Raw (ms) | GPU Raw p50 / p95 (ms) | GPU Clean (ms) | Clean Avg (ms) | Ingest Mean (ms) | Network Mean (ms) | Pack Mean (ms) |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| Frame 600 | `8.03` | `7.97 / 8.06` | `8.03` | `7.97` | `0.045` | `7.43` | `0.024` |
| Frame 1200 | `7.82` | `7.82 / 7.93` | `7.82` | `7.82` | `0.045` | `7.83` | `0.005` |
| Frame 1800 | `7.92` | `7.93 / 8.03` | `7.92` | `7.91` | `0.046` | `7.82` | `0.001` |
| Frame 2400 | `7.81` | `7.75 / 7.87` | `7.81` | `7.75` | `0.046` | `7.82` | `0.002` |
| Frame 3000 | `8.10` | `8.08 / 8.18` | `8.10` | `8.05` | `0.043` | `7.82` | `0.026` |
| Frame 3600 | `8.43` | `8.02 / 8.18` | `8.01` | `7.96` | `0.046` | `7.83` | `0.043` |

### 3.2 《影之刃零 PBZ》 (`2562x1441 -> 3840x2160`, D3D12 Direct Fast)

| 采样帧号 | GPU Raw (ms) | GPU Raw p50 / p95 (ms) | GPU Clean (ms) | Clean Avg (ms) | Ingest Mean (ms) | Network Mean (ms) | Pack Mean (ms) |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| Frame 600 | `14.56` | `14.56 / 15.56` | `11.97` | `11.96` | `0.049` | `11.86` | `0.001` |
| Frame 1200 | `12.21` | `12.22 / 19.99` | `12.21` | `11.97` | `0.053` | `11.90` | `0.004` |
| Frame 1800 | `12.03` | `12.06 / 16.79` | `12.03` | `12.10` | `0.063` | `12.03` | `0.002` |
| Frame 2400 | `11.95` | `12.03 / 12.33` | `11.95` | `12.06` | `0.062` | `12.01` | `0.007` |

### 3.3 《红色沙漠 Crimson Desert》 (`1920x1080 -> 3840x2160`, D3D12 Direct Fast)

| 采样帧号 | GPU Raw (ms) | GPU Raw p50 / p95 (ms) | GPU Clean (ms) | Clean Avg (ms) | Ingest Mean (ms) | Network Mean (ms) | Pack Mean (ms) |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| Frame 600 | `7.33` | `7.43 / 7.92` | `7.33` | `7.34` | `0.038` | `7.24` | `0.003` |
| Frame 1800 | `7.30` | `7.35 / 7.96` | `7.30` | `7.34` | `0.036` | `7.20` | `0.007` |
| Frame 3000 | `7.91` | `7.46 / 8.94` | `7.47` | `7.44` | `0.042` | `7.37` | `0.025` |
| Frame 4200 | `7.54` | `7.63 / 7.88` | `7.54` | `7.66` | `0.044` | `7.63` | `0.001` |
| Frame 6000 | `7.72` | `7.66 / 7.93` | `7.72` | `7.66` | `0.045` | `7.62` | `0.000` |
| Frame 7200 | `7.76` | `7.66 / 8.01` | `7.76` | `7.66` | `0.044` | `7.55` | `0.001` |
| Frame 8400 | `8.05` | `7.53 / 7.91` | `7.43` | `7.46` | `0.042` | `7.35` | `0.002` |

---

## 4. 本地未跟踪原始日志归档索引 (`docs/test-evidence/game-logs/2026-11-09-initial-baseline/`)

以下原始日志文件保存在本地仓库 `docs/test-evidence/game-logs/2026-11-09-initial-baseline/` 下备查（已加入 `.gitignore` 不纳入版本控制）：

| 相对路径 | 大小 (Bytes) | SHA256 校验和 |
| :--- | :---: | :--- |
| `cyberpunk-2077/openrr-amd.log` | `4,415` | `ca83da5fecb46fb8ee3acb13938906edd6cb411a1fa2988ef520d9250c9144e8` |
| `cyberpunk-2077/dlssnr-amd.log` | `168,056` | `65cf70e48d8303ec36e38efcc422853c2d9fbd04ccc6b8aeffad0f7653ced981` |
| `cyberpunk-2077/openrr-amd.ini` | `1,795` | `07246ea7b8547ffe40d6dfb240ad8be9e1624490430a9f2a03006f9b922b91e0` |
| `cyberpunk-2077/system.txt` | `3,301` | `0dd42bc4909addc9c4e3fa47967a9d957ce78b2be773b2494e0e1276ce832596` |
| `phantom-blade-zero/openrr-amd-2892frames.log` | `4,578` | `4c4dd36dbac4c744854bdd6a98f71a7a6038b17c9e969600aef529c0495af213` |
| `phantom-blade-zero/openrr-amd.log` | `3,600` | `2086e37c1deb80512a57742d683d929ce0b752df3d9b6f9da5a30246005b6d23` |
| `phantom-blade-zero/dlssnr-amd.log` | `12,415` | `07e64879c5df54365e8c3d5d968383eceb0df4f34fd83d9a68551f006021d1f0` |
| `phantom-blade-zero/opennr_rr-history.log` | `24,542` | `a4b26ac305253c075bbe111d3080e164851e8a63c4075ca9d891ed04a32c7835` |
| `crimson-desert/openrr-amd.log` | `6,132` | `c33a154d07b0b36cb48ed39e8bc952a3dd4e03fbfb8f412849aea249b04332b3` |
| `crimson-desert/dlssnr-amd.log` | `62,565` | `effdbf9e3739e2a0093d6dc26f021bca73dc8ead2afe528753fa37b84ca9b8ae` |
