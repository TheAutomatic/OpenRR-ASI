# 原始 ACO 与保留组合的 4K 对照 — 2026-11-10

2560×1440 → 3840×2160，Fast，Windows D3D12/PAL，global/vector8，
direct guides/output、sampler、flow 均为1。固定相同诊断 runtime，
仅切换原始shader/记录与db50380交付组合的shader/记录。不是LLPC对照。
原始记录使用已验capturev4 global/vector8，未使用stage内旧v3 manifest。
两侧各自data/aco，清除ONR_ACO_DIR及direct/sampler/trace/stamps覆盖项。

先8 warmup+8 steady+12 DRS逐帧正确性：20帧全部逐位一致、无非有限。
性能测量关闭逐帧读回、trace及stamps，48 warmup+240 steady+64 DRS，
共6对，交替旧/新顺序，汇总每次run中位数的中位数。

| 阶段 | 原始ACO ms | 当前组合 ms | 耗时变化 | 改善对数 |
|---|---:|---:|---:|---:|
| 稳态 | 13.96475 | 13.62165 | -2.4569%（-0.34310ms） | 6/6 |
| DRS | 13.16905 | 12.66835 | -3.8021%（-0.50070ms） | 5/6 |

稳态逐对变化：-5.2262%、-2.5519%、-2.3491%、-2.4900%、-2.6194%、-2.3325%。
原始run范围13.9292–14.3967ms，组合13.5882–13.6443ms。首对原始
较慢，后五对稳态改善集中2.33%–2.62%，没有删除首对。
DRS逐对变化：-8.8619%、-3.9673%、-1.4333%、-2.3632%、+0.4445%、-4.5201%，
波动更大，不将3.80%描述为每个DRS过程的固定增益。

该结果与1080p组合稳态约2.55%改善方向一致，没有新LLPC测量，
不能与历史4K LLPC数据拼接成最新同批差距。没有扩大性能优化搜索。

复现：

```powershell
& asi/tests/run_aco_screen.ps1 `
  -Candidate build/aco-runtime-trace/comparison-4k/latest `
  -Baseline build/aco-runtime-trace/comparison-4k/original/data `
  -Name combination-4k -Stage Correctness -PerDataRecords `
  -RenderWidth 2560 -RenderHeight 1440 -OutputWidth 3840 -OutputHeight 2160
# 正确性通过后改为 -Stage Long；新复测必须换Name，防止覆盖。
```

两套data来自原始stage的shaders+aco-review-v4/global-vector8记录，
以及asi-delivery-candidate/stage/openrr-amd。日志保存实际SPIR-V/code身份。
原始summary、日志、frame-state、order、输出哈希归档于
mesa-aco-next-optimization-2026-11-10/combination-4k/。
40个逐帧可再生读回在归档SHA256后按清单清理，回收2,654,208,000B
（2.47GiB）；清单在同证据根cleanup-4k-files.json和summary。
本次新增分辨率参数与报告不改变生产 shader 或 ACO 记录；于 2026-11-11 分层迁移前归档提交。
