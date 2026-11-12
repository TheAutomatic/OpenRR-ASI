# 锚点 #2 首轮对账：OpenRR vs RTX 5090 `nvngx_dlssd.dll 310.9.1.0`（rr_contrast_v1 全量 + v2_motion 预演）

分支 `codex/anchor2-rtx5090-parity`（自 `codex/mesa-263-comparison` @ `4510cc1` 切出，本地未推送）。
被测产物 = 当前工作树 `4510cc1` 对应的 `build/asi-015-aco/stage`（ACO 记录集 = c8ca717 修复栈）。
工具：`runtime/bench/openrr_contrast_eval.cpp`（新，经 `build_runtime_trace.sh` 增量构建为 `build/contrast-eval/openrr_contrast_eval.exe`），只读 `#include` RR_Contrast（`v2-motion @ 7a8c2bd`）的 `rr_ref_scenes.hpp`，GPU 前强制逐用例哈希门禁。

## 0. 哈希门禁（同源输入合成）

| 套件 | 门禁 |
|---|---|
| v1（29 用例） | **76/76 PASS**（含 T-case 的 drs_f064/f072 定帧号语义） |
| v2_motion（21 用例） | **44/44 PASS**（P13b 的 `manual_exposure_tex_step` 未序列化进 cases.json，按 `kGuideExposureTex` 位推断——参照 runner 源码 `ngx_rr_bench.cpp:513`，属基准仓序列化缺口，已按其原始定义对齐） |

## 1. 工具与执行口径

- 三档配置 ×（`02_quality_core` 6 组 + `04_quality_ext` 4 组 + `05_probes` 13 组），`Legacy(fast=0,LLPC)` / `Fast-LLPC(fast=1,LLPC)` / `Fast-ACO(fast=1,ACO)`，共 23 用例 × 3。
- `Create` 后以 reset 帧 priming 至 `ONR_RR_STATE_READY`（+fast 内核建毕）才进 f000——无任何用例帧落入 BUILDING 双线性回退（对照 5090 侧 `initial_ngx_result_hex=0x1` 语义）。
- 指标与 `ngx_rr_bench` 的 `CompareRgba16f` 同口径（RGB clamp [0,1] PSNR01），另加 reinhard PSNR / max_ulp / exact% / non_finite；内存比对，无 GB 级落盘。
- 每用例帧循环严格复刻基准 manifest：steady N 帧（f0 reset）→ DRS 16 帧（4 低 4 高交替、di≥8 且 %4==0 时 reset，即 f072/f076）。

## 2. 问题 A：S0 跨厂商真值锚点基线（g0）

`Legacy` vs RTX 5090（PSNR01 dB，关键帧；三档完整曲线见 `anchor2-v1/*-per-frame.csv`）：

| 用例 | f000 | f015 | f063(稳态末) | drs f067 | drs f075 |
|---|---:|---:|---:|---:|---:|
| Q_S0_1080p_qual | 48.93 | 35.65 | **28.97** | 47.80 | 47.80 |
| Q_S0_1440p_qual | 48.96 | 33.44 | **28.94** | 47.63 | 47.63 |
| Q_S0_1080p_dlaa | 48.66 | 46.31 | **31.11** | — | — |
| Q_S0_4k_ultrap | 48.70 | 35.84 | **29.12** | — | — |
| Q_S0_4k_qual | 46.98 | 37.40 | 39.77 | 40.86 | 40.86 |
| Q_S0_4k_perf | 48.22 | 47.74 | **48.14** | 48.72 | 48.72 |
| Q_S1_1080p_qual | 43.69 | 42.93 | 42.98 | 39.95 | 39.59 |
| Q_S2_1080p_qual | 40.64 | 39.29 | 39.21 | — | — |

- **跨厂商地板 ≈ 29 dB（S0 稳态 g0 @1080p/1440p/ultrap）**：与 §四锚点体系里 Fast-vs-Legacy g0 地板（49.5）同源但更深的根源——两边是完全独立的实现，S0 稳态是强混沌累计态。f000（reset 后首帧，历史清空）落在 **46.9–49.0 dB**，DRS reset 帧（f067/f075，历史重建）47.3–48.7 dB——"无历史/短历史"状态跨厂商一致性高一个数量级，长历史累计才是分歧主体。
- **Fast-LLPC 与 Fast-ACO 相对 5090 无额外系统性偏离**：三档逐帧差 ≤0.5 dB（个别瞬态如 dlaa f063 LLPC 36.08 / ACO 35.32），ACO 与 LLPC 的差远小于两者与 5090 的差。`non_finite == 0` 全表成立。

## 3. 问题 B：P07/P08（DoF × Preset E/F）

- **OpenRR 的 P07 与 P08 逐位一致**（三档全部：如 fast-aco 两者的 vs-P00 指标完全相同 `29.5903/1865/6101667`）——`OnrRrCreateInfo/Frame` 无 preset 输入，preset_hint 只进哈希不进引擎；ASI 侧 `rr_asi_main.cpp:1577-1582` 把全部六个 NGX preset hint 置 0。DoF 引导本身走 `depth_of_field_guide → attr 32`（`enc0_attr.comp` signed_log 路径），与 preset 无关。
- 因此 OpenRR 单输出同时对着 5090 的两个 preset 网络输出比：**vs 5090-P07(presetF) 23.39-23.44 dB，vs 5090-P08(presetE) 22.19-22.26 dB**（Legacy/Fast 几乎同值）——OpenRR 落在 5090 两 preset 之间略偏 F，无"对齐错一个 preset"的问题；5090 P07/P08 互差（20.8 vs 22.7 的 vs-P00 响应）是网络选择差异，OpenRR 结构上不可能复现。
- P12（preset_hint=0）≡ P00 逐位一致（999 dB）✓ 旁证 preset 完全不入引擎。

## 4. 问题 C：P02（v1 反置 Opacity 交叉验证）

- 5090 侧 P02 vs P00 = 6.170 dB（玻璃外 `Opacity=1.0` 病态输入）；**OpenRR 自身 P02 vs P00 = 6.1759/6.17524/6.17526 dB（Legacy/LLPC/ACO）——响应幅度与 5090 一致到 0.006 dB**，同一病态输入被两边以同量级放大。
- **OpenRR P02 vs 5090-P02 = 56.9/56.6/56.6 dB（三档）**：在两实现彼此只差 ~29 dB 的全局地板下，P02 这一病态路径上双方的一致性反冲到 56+ dB——反向验证了 `downsample_tl` 的层合成公式与官方高度同构（错误处理 Opacity=1.0 的方式一致）。
- v1 P02 ≡ P03（OpenRR 逐位；5090 同样 6.170 一致）：v1 静态玻璃下 tlmv 无稳态可分辨贡献；v2（运动蚀刻玻璃）P02 vs P03 = 43.5 dB 才分开 ✓。

## 5. 问题 D：4K Perf DRS 下界与奇数子矩形 P11

- `Q_S0_4k_perf` DRS（1632×918 交替）：5090 曾在 DRS 段 `0xBAD00005`→recreate；**OpenRR 无失败概念**，DRS 段 f067/f075 = 48.72（Legacy）/48.58（Fast）dB，与该用例稳态（48.14）同档——最低 DRS 分辨率不触发额外退化。
- `P11`（1281×721 奇数子矩形）：42.69/42.39/42.37 dB（三档），与 P00 的 42.9 同档（−0.3~0.6 dB 为奇数网格重采样本身的贡献），无崩溃无非有限。

## 6. v2_motion 预演（无参考目录冒烟，44/44 哈希 PASS）

探针自等价（f015 vs P00，fast-aco；`v2-smoke-probes-self.csv`）：

| 探针 | vs P00 (dB) | 判读 |
|---|---:|---|
| P01 disocc | 56.95 | 响应正常（v2 内容下比 v1 的 50.9 更接近） |
| P02 transp rgba | 28.68 | v2 运动玻璃+正确 opacity 语义下响应增强 ✓ |
| P04a/P04b（sss/cbt 拆分） | 50.08/50.28 | 两半各自独立响应，量级一致 ✓ |
| P05 spec mvec / P09 / P10 | 49.90/49.90/41.80 | ✓ |
| **P13a/P13b exposure** | — | `abi_not_exposed_yet`：`OnrRrFrame` 无 Pre.Exposure/Exposure.Scale/1×1 ExposureTexture 字段，工具诚实跳过 GPU（自等价表中的 999 为空像素占位，非结果） |
| **P14 subrect base (32,24)** | **999（逐位）** | 洋红哨兵边框未被读入 → **OpenRR 正确尊重 Input Subrect.Base** ✓ |
| **P15 mv_scale=−0.5** | **999（逐位）** | ⚠ **发现：OpenRR 未对主运动向量施加 MV.Scale**。源码核查：`engine.cpp:115` 的 `C(0x320/0x324) = mv_scale·(ix/rx)` 只乘 SpecularMvec（`enc0_attr.comp:492`）；主 mv_near/mv_c/mv_far 原始消费（`:513-519` 只乘内部尺度 0x2f8）；TransparencyLayerMvecs 有 `tlmv_scale`（`engine.cpp:852`）。与 NGX 语义（MV.Scale 乘全部 mvec）存在缺口——真实游戏若传非 1 的 MV.Scale 会得到错误重投影。建议列入引擎待办（今晚未改生产代码）。 |
| P16a（flag=0x4f，MV 内容=P00） | 999 | MV_JITTERED 标志置位但内容未变 → OpenRR 输出不变：要么忽略该标志，要么其默认约定已按 jittered 处理（与 jsign=0 的内容一致）；**待 v2 5090 参考到货裁决**（NGX 侧 P16a 应有响应） |
| P16b / P16c（±jitter delta） | 32.95 / 32.25 | MV 内容变化被正确放大传播 ✓ |
| P17 unpacked roughness | 999 | normals.w=0 + 独立 R16F 下输出与 P00 逐位一致：要么 roughness 独立纹理路径正确且 roughness 内容在该帧无差异…实际上 P17 的 rough 内容与 P00 的 normals.w 相同数值，仅布局不同——**逐位一致 = 布局迁移路径正确** ✓ |

## 7. 结论

1. **对账管线已闭环**：同源合成（76+44 哈希全过）→ 三档驱动 → 内存指标 → 与 5090 归档逐帧对账，全部无落盘大文件；v2 参考到货后一行复跑（见 §8）。
2. **跨厂商基线**：S0 g0 稳态地板 ~29 dB、reset/短历史帧 47–49 dB、S1/S2 ~39–44 dB、病态路径（P02）反冲 56 dB——这是 OpenRR 实现正确性之外、由"独立实现"决定的可重复数字基线。
3. **三档相对 5090 无编译器级额外偏离**（ACO ≈ LLPC ≤0.5 dB）。
4. **新发现（记录不改码）**：主运动向量缺 MV.Scale 施加（P15 逐位暴露，源码定位 `engine.cpp:115` 消费面不完整）；MV_JITTERED 语义待 v2 参考裁决；ABI 缺口 P13a/b（曝光标量/纹理）。

## 8. 复现命令

```powershell
# v1 三档全量（本轮实际执行）
build/contrast-eval/openrr_contrast_eval.exe --asi build/asi-015-aco/stage/plugins/openrr-amd.asi `
  --data-dir build/asi-015-aco/stage/openrr-amd --dlssd <nvngx_dlssd.dll> `
  --suite v1 --config legacy|fast-llpc|fast-aco `
  --ref-dir <RR_Contrast_RTX5090_310.9.1.0>/extracted/results --out <dir>
# v2_motion 参考到货后（唯一变化：--ref-dir 指向 v2 结果目录 + --suite v2_motion，跑全 21 用例去掉 --case）
```
