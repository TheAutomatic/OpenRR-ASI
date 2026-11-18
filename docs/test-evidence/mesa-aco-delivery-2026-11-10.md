# ACO 已验证组合交付整理 — 2026-11-10

本轮停止扩大性能搜索，保留 enc96 单 tile、dec96 FC1 整数地址、
enc128 单个 MLP 循环 DontUnroll，以及 dec96/enc128 有界 cmat 地址记录。
阶段性收尾不表示达到性能极限，也不表示消除 LLPC/ACO 差距。
工作树仍在 codex/mesa-aco，基准提交 ba817e3a4a47ec67108c3583f2af024ab591a6d0；
本轮没有提交、合并或替换原始 stage / 发布包。

## 最终改动清单

完整逐文件 SHA256 分类见相邻 change-inventory.json。它区分本轮改动、
长期工具和已提交的构建基础；不是把整个历史 ACO 接入都算作本轮新改动。

| 类别 | 文件与作用 |
|---|---|
| 生产 shader | enc_stage_96_128_128.comp：仅 Fast flow/lite 改单 tile，保持 W2 reciprocal 策略；dec_dfn_96_128.comp：仅同类变体启用 FC1 row/tile 整数表达式；enc_stage_128_128_160.comp：仅目标变体的 MLP_SLICE_LOOP=DontUnroll |
| 生产公共 include | onr_block.glsl 增加 FC1_TILE 且未选核保持原表达式；onr_block_fast.glsl 增加局部 loop hint 与保留 reciprocal；onr_block_w2.glsl 使用 FC1_TILE。其他 shader 的最终 SPIR-V 与已测组合保持一致 |
| Mesa 附加补丁 | asi/build/mesa/prepare_global_address.py 及 global-address.patch；在 capturev4 后应用，仅有界 wave32 FP16 column-major global load，保留 64 位基址与有符号源索引；不扩大至 store/LDS/动态 stride |
| 构建配置 | build_capture.sh 记录附加 patch hash；build_aco_records.py 精确选择、指纹、manifest、记录完整性与冲突检查；build_asi.sh 明确两个 stem、默认 global/vector8、capture 移至 stage 外，并复制 LICENSE/THIRD_PARTY.md |
| 构建及部署文档 | asi/build/mesa/README.md、README.md、THIRD_PARTY.md；部署说明补全 ACO 目录与外部权重边界 |
| 长期验证工具 | run_aco_screen.ps1 / summarize_aco_screen.py / cleanup_aco_screen.ps1；capture_llpc_hotspots.cpp / hotspot_resources.py；cmat_address_boundaries.py；verify_retained_combination.py；本轮增加 test_aco_build_policy.py、verify_aco_delivery.py、run_aco_delivery_smoke.ps1、build_delivery_probe.sh、归档管理及证据冻结入口 |
| 原有关键诊断入口 | compare_runtime_trace.py、test_runtime_trace_compare.py、test_aco_records.py、test_asi_units.cpp、aco_abi.comp、runtime/bench/openrr_bench.cpp 保留；build_runtime_trace.sh 增加独立 base/out，run_runtime_trace_batch.ps1 增加 TraceTools 和 Legacy 控制选项 |
| 一次性实验工具 | 59 项逐名列于 experiment-tool-list.json，归档 experiment-tools.zip 后从 asi/tests 移除；不按通配符删除 |

生产改动准备纳入构建，验证工具不部署到游戏目录。调试符号、benchmark、
单测、Mesa、noop shim、capture、occupancy/B2 输入均为构建产物。
常规构建仍生成 occupancy/B2 诊断变体及其记录，没有删除已有构建功能。

## 两个地址选择与缓存规则

build_asi.sh 默认 OPENRR_BUILD_ACO=1、precision=global、matrix_loads=vector8，
明确传入且仅传入：

- dec_dfn_96_128_fl_lt
- enc_stage_128_128_160_fl_lt

记录工具清理继承的 ONR_ACO_CMAT_ADDRESS，再按精确 stem 设置。工具自身的
local/scalar 默认值不代表常规包默认值；手工调用必须显式传 global/vector8。
OPENRR_ACO_PRECISION/MATRIX_LOADS 是显式实验覆盖项，使用后需重新验证。
输入缺少所选 stem 或工具链未声明地址补丁时直接失败。相同 SPIR-V 字节
不能以不同 stem 请求冲突地址策略，工具拒绝这种输入。

离线缓存指纹覆盖 capture 可执行文件、记录脚本、整个 toolchain.json、
precision、matrix_loads 与排序后的精确 stem 列表。toolchain 包含 pinned
Mesa 提交、RADV 库 SHA256、capture.patch 和 global-address.patch SHA256。
策略或工具链变化使整批旧指纹失效；SPIR-V 变化选择新的 FNV 记录键。
--refresh 强制捕获。已有记录还必须匹配 manifest 的 record_sha256；旧版
未记录 SHA256、文件缺失或损坏均重新捕获。manifest 逐核记录 cmat_address，
MessagePack 索引绑定 JSON manifest hash、SPIR-V/code hash。运行时继续检查
记录哈希/ABI/目标，不读取 build-only capture。

这次没有修改运行时缓存格式：PAL clear_range 容器仍按原驱动身份及 SPIR-V
缓存，每次 ACO 导入前读取并校验当前记录，再替换代码与资源创建 PSO；
没有把旧网络 ACO PSO 当作可跨新记录复用的磁盘缓存。

23 项 CPU 测试通过，其中 5 项新增测试覆盖精确启用及继承环境清理、策略与
工具链失效、损坏记录重捕获、缺失选择/补丁拒绝、相同 SPIR-V 冲突拒绝。
实际常规再次构建没有重捕获行；再运行同一记录步骤，824 个 capture 文件
内容和 mtime 全部不变。证据 cache-check.json、build-final.log、cpu-tests.log。

## 常规构建的独立候选

在 WSL 仓库根使用：

```bash
bash asi/build/build_asi.sh build/asi-delivery-candidate
python3 asi/tests/verify_aco_delivery.py \
  --out docs/test-evidence/mesa-aco-delivery-2026-11-10/package-audit.json
```

交付目录为 build/asi-delivery-candidate/stage。构建使用本机已有固定 Mesa
源码与依赖，无下载；WSL/noop 只离线编译，运行检查为 Windows D3D12/PAL。
正式 build/asi/stage 和原发布包没有覆盖。使用新输出目录避免混入旧记录或日志。

最终范围为 199 个文件：1 个 ASI、1 个 INI、2 个许可说明、90 个 runtime
SPIR-V、103 个 NRPAL002 记录及 2 个 manifest。103 中 90 个覆盖 runtime，
另外 13 个覆盖常规构建的 occupancy/B2 不同哈希；不将它们误写成 103 个
runtime shader。原始 capture 在 build/asi-delivery-candidate/aco-capture。
不包含权重 DLL、OptiScaler、安装器或 Linux 库。

ASI 为 x64 PE、3,322,132 B，保留常规 API 导出，无 OnrRrDiagnostic 测试导出；
导出/依赖列表见 binary-exports-imports.txt。候选包、逐文件 SHA256 清单为
candidate-stage.zip / candidate-stage.json，源码与长期工具为
delivery-source-tools.zip / change-inventory.json。这些是独立审查候选，
未部署也未覆盖正式包。构建时间戳可能使 ASI 重建字节不同；不宣称 PE
全字节可复现，要求 shader/记录身份可复现并对最终 PE 单独保存哈希。

包审计实际检查：

- 90 个 runtime SPIR-V 及 NRP 与先前已测组合逐字节一致。
- 三项保留代码身份：enc96 f3817f3fb0a26545 / c08996618a61cd08；dec96 607b418f7ed5346e / 6a4ccda0cd2efb14；enc128 7637570aac01b415 / 591c303d1916df59（SPIR-V / code FNV）。
- 全部常规输入哈希与 103 个 manifest 记录完整对应，NRP 长度、代码 hash 与文件 SHA256 一致；MessagePack 与 JSON 一致。
- 默认策略和两个精确选择正确；包文件白名单无 capture、诊断 shader、Linux 库或单测。INI 与许可说明匹配源码。

## 包级验证及未覆盖项

已完成新链接单测的独立 ABI 夹具：244-byte push/grid、原 lane-varying
selftest、BDA clear、采样描述符槽 0/3/7。探针夹具放在 stage 外，因常规
90 个 runtime shader 不包含 aco_abi，未为通过探针扩充交付范围。

随后直接加载常规新 ASI，通过默认 compiler=0，使用包内记录、不设置
ONR_ACO_DIR。Fast/Legacy 各 8 warmup+8 steady+12 DRS，20 个输出逐帧
有限值检查通过，实际 direct_guides/output/sampler/flow 均1，日志保持
全 ACO，Fast 三项代码身份匹配。输出哈希、日志、summary 存在
package-smoke-delivery/。早先显式 strict-ACO 冒烟也保留。

这些无 LLPC 对照的包级单上下文运行只证明新模块可加载、执行及输出有限；
summary 中空 parity 对象不视作新的数值等价证据。常规 ASI 无相位对齐
诊断导出，因此不以该冒烟重测相位对齐性能或替代既有逐帧精度证据。

必要的数值证据复用已测同字节组合：292 帧相对上一保留版本逐位一致；
Fast LLPC/ACO 20 帧逐帧质量通过，Legacy 20 帧逐位一致；第6帧两侧各69
个读回及最终输出复现原案例，traced/untraced 一致。首次非零仍是
Engine6 dispatch3 d3d_enc0_attr_f_lt，属性9、像素203/378、字节23234111
的 FP16 midpoint（LLPC0x6e、ACO0x6f），不归因于本轮改动。

交付审查后、替换正式 stage 前，仍需以下包级项目；本轮未冒充已完成：

| 项目 | 当前状态与必要动作 |
|---|---|
| OptiScaler/游戏实际安装、INI/UI切换、销毁重建与设备生命周期 | 未对这个候选重跑真实游戏；使用候选独立部署目录，确认加载包内 ASI/records 与外部权重 |
| 外部依赖组合 | 已在当前 RX9070XT、Windows 驱动26.10.1、现有权重上冒烟；未覆盖其他驱动、其他GPU、缺失/错误权重与安装器 |
| 分辨率/功能面 | 复用已验720p→1080p及DRS；未为候选遍历更多分辨率、全optional guides、flow关闭、间接部署路径或Vulkan原生路径 |
| 完整生命周期/负例套件 | CPU23项与专项ABI通过；本轮未运行全部ASI GPU生命周期套件、所有坏包运行时负例或长时间游戏稳定性 |
| 发布动作 | 无提交/合并/正式stage替换；待用户审查后决定，不自动部署 |

## 性能结果的四个边界

| 测量对象 | 结果 | 可用结论 |
|---|---|---|
| enc128 插桩单核，两轮各596样本 | 0.1214→0.0740 ms；0.1213→0.0744 ms，约39% | 定位目标循环收益；不能作发布端到端增益 |
| 无插桩 enc128 增量，相对上一轮含dec96地址优化组合，独立六对 | 稳态3.69404→3.66118 ms，-0.890%；DRS3.49100→3.44710 ms，-1.258%；均6/6改善 | 本轮单因素保留依据；有CPU并行构建的较大数字未作主要结论 |
| 完整组合相对原始ACO，直接六对 | 稳态3.76560→3.66970 ms，-2.547%；DRS3.54452→3.47048 ms，-2.089%；均6/6改善 | 同批直接实测，不能相加各轮收益替代 |
| 最终组合对LLPC最新直接性能 | 没有做 | 旧LLPC/ACO表已接受，但不可与新ACO异批数字拼接成新差距百分比；没有证明消除差距 |

当前已测试方向的边际收益有限，组合值得保留，停止扩大搜索。没有将小幅
端到端变化证明成内存/同步瓶颈，也不宣称优化上限。原始测量见
mesa-aco-enc128-unroll-2026-11-10.md 及相邻证据。

## 复现入口与一次性工具归档

组合离线重建与包审计使用上述常规命令；包冒烟：

```bash
bash asi/build/build_delivery_probe.sh build/asi-delivery-candidate
```

```powershell
& asi/tests/run_aco_delivery_smoke.ps1 -Output build/aco-delivery-review/new-smoke
```

关键问题诊断保持独立路径：

```bash
OPENRR_TRACE_BASE=build/asi-delivery-candidate \
OPENRR_TRACE_OUT=build/aco-delivery-trace bash asi/build/build_runtime_trace.sh
```

```powershell
& asi/tests/run_runtime_trace_batch.ps1 -TraceTools build/aco-delivery-trace `
  -Output build/aco-delivery-trace/repro -Stage Baseline `
  -DataDir build/asi-delivery-candidate/stage/openrr-amd `
  -Records build/asi-delivery-candidate/stage/openrr-amd/aco/gfx1201/openrr-d3d12-v1
# 检查三组控制后，沿用同样参数加 -Stage Trace -TraceFrame 6。
```

```bash
python3 asi/tests/compare_runtime_trace.py \
  build/aco-delivery-trace/repro/fast-trace-6/baseline-trace-6 \
  build/aco-delivery-trace/repro/fast-trace-6/candidate-trace-6 \
  --out build/aco-delivery-trace/repro/trace6-comparison.json
```

原始完整第6帧仍在 build/aco-runtime-trace/runs-approved-2026-11-10/
combination-fast-trace6；已归档日志与读回哈希位于 enc128-unroll 证据目录
的 combination-correctness/。compare_runtime_trace 仍明确要求资源/偏移
核对，不能单凭不同分配地址或未写padding判断数值源头。

59项一次性脚本先写入 experiment-tools.zip，验证归档 SHA256、成员集合、
各成员 SHA256 与移除前源码一致，再按逐名清单移除；没有通配符删除。
长期工具保留在 asi/tests，关键组合及首次分歧的入口无需恢复实验脚本。
历史实验按下列步骤恢复原路径，之后读取对应历史报告命令：

```bash
python3 asi/tests/manage_aco_experiment_archive.py verify
python3 asi/tests/manage_aco_experiment_archive.py restore
# 历史实验完成后，只有文件仍与归档一致时才允许再移除：
python3 asi/tests/manage_aco_experiment_archive.py prune
```

管理器先验证全部成员，已有同名文件发生修改则停止，不覆盖修改。12份
历史文档已增加恢复入口，清单 updated-reproduction-docs.json。归档包含
脚本而非所有可再生大dump，历史命令所需数据应从各报告证据归档恢复。
本轮没有扩大GPU性能矩阵；包冒烟读回在逐文件记录哈希后清理，审查
证据及此前关键案例保留。

构建整理中的两次失败日志一并保留：首次 capture 移出stage后未先创建
记录目录，已修复并纳入CPU测试；一次重复构建执行期间编辑脚本造成尾部
读取错误，冻结脚本后常规 build-final.log 完整成功。它们不是GPU精度失败。
23项CPU测试、PowerShell解析、bash语法与git diff --check通过；最终
候选包与源码归档校验见相邻JSON。等待交付审查后再决定提交和stage替换。
