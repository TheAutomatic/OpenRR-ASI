# 当前 ACO 与 LLPC 的 4K 直接对照 — 2026-11-10

本测试纠正前一次把用户的新旧4K要求理解为旧/新ACO的范围错误。
本次直接比较 Windows D3D12/PAL LLPC=1 与当前交付ACO=3，
两侧使用完全相同的当前SPIR-V，ACO采用db50380已验证组合记录。
不是把旧LLPC数字与新ACO数字跨批拼接。

2560×1440 → 3840×2160，Fast，global/vector8，flow/direct guides/
direct output/sampler均1，相位对齐。固定相同runtime，只改变编译器。
先8warmup+8steady+12DRS逐帧检查，再6对48warmup+240steady+64DRS。
性能阶段无逐帧读回、无trace/stamps，交替LLPC/ACO顺序。
六对日志按order角色核对LLPC标记及当前ACO enc128代码591c303d1916df59。

| 阶段 | LLPC ms | 当前ACO ms | ACO耗时增加 |
|---|---:|---:|---:|
| 稳态 | 12.24080 | 13.80005 | +1.55925ms，+12.7381% |
| DRS | 11.50740 | 13.12290 | +1.61550ms，+14.0388% |

汇总为各run中位数的中位数，六对两个阶段都是LLPC更快。
稳态LLPC范围12.0966–12.5844ms，ACO13.6553–14.0552ms；
逐对增加12.8854%、10.0657%、11.6875%、14.6752%、12.5344%、13.5302%。
DRS逐对增加11.0289%、9.3424%、10.3683%、16.7688%、19.5827%、13.3269%，
DRS波动较大，汇总值不表示每次切换固定相差14%。

20帧全部通过既有Fast质量阈值，均无非有限值；最低PSNR_01=60.9627dB，
最大164ULP。不是逐位一致，也没有放宽阈值。本轮不追溯4K首次分歧，
不将1080p首次分歧案例直接认定成4K全部误差解释。

复现：

```powershell
& asi/tests/run_aco_screen.ps1 `
  -Candidate build/aco-runtime-trace/comparison-4k/latest `
  -Baseline build/aco-runtime-trace/comparison-4k/latest/data `
  -BaselineCompiler llpc -Name llpc-current-4k -Stage Correctness -PerDataRecords `
  -RenderWidth 2560 -RenderHeight 1440 -OutputWidth 3840 -OutputHeight 2160
# 通过后改为 -Stage Long；复测换Name，不能覆盖原始数据。
```

latest/data来自build/asi-delivery-candidate/stage/openrr-amd。
原始summary、日志、frame-state、order和全部输出哈希保留于
mesa-aco-next-optimization-2026-11-10/llpc-current-4k/。
40个可再生逐帧读回在哈希归档后清理，清单cleanup-llpc-4k-files.json。
本轮修改只增加测试脚本参数及证据，生产shader/记录未改、未提交。
