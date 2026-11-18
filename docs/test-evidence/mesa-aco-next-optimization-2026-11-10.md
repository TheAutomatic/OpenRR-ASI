# ACO 第二轮离线筛选与 runtime 验证 — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


保留 dec_dfn_96_128_fl_lt 的 FC1 整数地址计算改进。两组各六对
Windows D3D12/PAL 测量分别得到稳态 -0.260% 和 -0.514%，DRS
-0.847% 和 -1.087%。收益小且存在波动，完整逐对数据保留。两个
W2 单 tile 短测均回退，停止扩展；enc128 地址变体短测结果混合，暂缓。

## 控制与执行路径

控制使用上一轮已保留的 enc96 修正版：
build/aco-runtime-trace/experiment-enc96-wave-rcp/data，enc96 SPIR-V
f3817f3fb0a26545。所有候选两侧均包含它；每个候选只改变一个目标核。
没有重复已接受的完整 LLPC/ACO 性能对照。

编译保持 pinned Mesa26.2.4、global/vector8、wave32。WSL/noop 用于
离线捕获；正确性和计时由 Windows openrr_bench.exe 执行 D3D12/PAL。
每次检查 direct_guides=1、direct_output=1、sampler=1、flow=1；
清理 ONR_RR_DIRECT 和其他相关环境开关。相位对齐、720p→1080p、
optional-guides=0。D3D12 两侧均 strict ACO；LLPC 仅用于第6帧正确性案例。

## 大文件清理

cleanup_completed_enc96.ps1 删除已完成实验的2504个可再生读回，
共33,240,223,104字节（30.96 GiB）。保留初始错误与修正后的
Engine139 trace、LLPC/ACO Engine6 trace，以及长序列关键输出
steady130/131/279。原始基线 trace、全部日志、summary、CSV、代码、
编译归档及逐输出哈希保留。

本轮结束再删除738个读回，回收12,242,534,400字节（11.40 GiB），
保留本轮完整Engine6 trace及长序列steady131/279、DRS11。合计
回收42.36 GiB；第二次清理时C盘空闲70.79 GiB。全部删除使用
已核对的工作树内绝对路径和逐文件清单。

清单和汇总在相邻证据目录的 cleanup-files.json、cleanup-summary.json、
cleanup-screen-files.json、cleanup-screen-summary.json。旧报告中的
“保留全部大读回”描述以本次清理清单为准；复现所需核心案例仍保留。

## 同一 SPIR-V 的 LLPC/ACO 资源表

[资源与代码体积表](mesa-aco-next-optimization-2026-11-10/hotspot-resources.md)
覆盖九个原始热点、上一轮保留enc96、本轮地址候选。每行严格配对
相同SPIR-V FNV及SHA256，JSON同时记录ACO代码哈希与LLPC ELF SHA256。
Windows驱动通过项目既有pipeline_elf导出LLPC ELF，无dispatch。

代码体积使用LLPC入口符号大小与ACO exec_bytes，排除ELF节、填充
及PAL适配前导码。资源是编译器导出值；没有将其说成已直接观测到的
Windows最终执行寄存器。SGPR、workgroup、wave、完整元数据和ELF
压缩包一起归档。

本轮dec96地址候选同SPIR-V比较：LLPC232VGPR、0scratch、9728B LDS、
20580B入口；ACO256VGPR、0scratch、9728B LDS、26488B入口。LLPC
入口体积及资源均与原SPIR-V行相同；ACO仍有较大的VGPR和代码体积。
这些数据用于选择后续优化方向，不能独立证明具体耗时原因。

## 离线并行候选与机器码筛选

两类候选独立准备，没有叠加。完整IR、源patch、SPIR-V、机器码、
资源、opcode计数、记录及哈希分别在W2与地址报告的编译归档中。

| 因素 / 目标核 | VGPR | LDS B/group | ACO入口 B | 编译后决定 |
|---|---:|---:|---:|---|
| W2→单tile / dec_dfn96_fl_lt | 256→256 | 9728→31744 | 27560→32692 | 淘汰 |
| W2→单tile / dec_dfn64_f_lt | 240→240 | 9728→17920 | 29096→17508 | 短测 |
| W2→单tile / dec_up64_f_lt | 256→240 | 9728→17920 | 31108→16996 | 短测 |
| W2→单tile / enc_stage64_f_lt | 256→256 | 9728→27648 | 36620→22656 | 暂缓 |
| FC1地址 / retained enc96_fl_lt | 240→240 | 19456→19456 | 38028→38028 | 机器码相同，淘汰 |
| FC1地址 / enc128_fl_lt | 240→240 | 21504→21504 | 37228→36652 | 短测 |
| FC1地址 / dec96_fl_lt | 256→256 | 9728→9728 | 27560→26488 | 短测后长测 |

上述候选scratch均为0；没有用代码缩小直接推断性能改善。W2的LDS
增长可能影响驻留与调度，短测只确认runtime结果，尚未证明具体回退来源。

### 数值预期

W2候选改变既有RR body的tile组织，保持K顺序、FP8/FP16/FP32边界、
显式FMA及激活策略。dec_up64显式保留原W2的反向window遍历；
enc64保留SIG_RCP规则。没有混入上一轮遇到的reciprocal策略变化。
不要求所有Fast候选bit-exact；预期无数值变化，因此若出现新差异，
必须解释其来源。两项实际短测候选均在20帧输出逐位一致，无非有限。

FC1候选只改整数地址。WT_FC1原先从线性offset恢复商余，已知row
按16对齐、tile为通道tile时直接使用 HID*C + row*C + tile*256。
880组CPU地址等价检查通过，加载的字节、矩阵顺序及精度规则不变。
dec96静态s_mul_hi_u32 26→14、s_mul_i32 63→39、s_lshr_b32 76→40、
s_sub_u32 31→19；WMMA、global与LDS操作数不变，VGPR和scratch不变。
这是减少冗余地址指令的证据；没有宣称提高了驻留wave或消除了spill。

参考Mochi的是检查lane/fragment不变偏移、形状相关tile与驻留成本、
按生成代码筛选的方法。本轮使用RR现有body和tiled权重布局，未复制NR
shader或权重打包，不假设其descriptor buffer结论自动适用于RR BDA。

## GPU分阶段结果

先8warmup+8steady+12DRS逐帧正确性，再16warmup+48steady+24DRS
两对轮换短测。正确性读回与计时分开，计时没有stamps或逐帧读回。
四项GPU候选的20帧全部逐位一致，无非有限；这是观测结果，并非
新增的统一Fast门槛。

| 候选 | 短测稳态 ms 控制→候选 | 稳态变化 | DRS变化 | 决定 |
|---|---:|---:|---:|---|
| W2 dec_up64 | 3.68678→3.72660 | +1.080% | +0.559% | 淘汰，不做长测 |
| W2 dec_dfn64 | 3.75778→3.80716 | +1.314% | +2.089% | 淘汰，不做长测 |
| FC1 enc128 | 3.68622→3.67672 | -0.258% | -0.935% | 稳态两对一升一降，暂缓 |
| FC1 dec96 | 3.69650→3.67828 | -0.493% | -0.785% | 升级长测 |

只有dec96进入8warmup+280steady+12DRS长正确性。292帧输出全部
逐位一致，无非有限。随后两组各六对48warmup+240steady+64DRS
轮换长测，报告各run中位数的中位数：

| 独立批次 | 稳态控制→候选 ms | 稳态变化 / 改善对数 | DRS控制→候选 ms | DRS变化 / 改善对数 |
|---|---:|---:|---:|---:|
| 第一组6对 | 3.72970→3.72002 | -0.260% / 4/6 | 3.53426→3.50432 | -0.847% / 5/6 |
| 第二组6对 | 3.74074→3.72152 | -0.514% / 5/6 | 3.52840→3.49004 | -1.087% / 5/6 |

第一组稳态控制范围3.71568–3.75164，候选3.70332–3.74712ms；第二组
控制3.72788–3.76068，候选3.70728–3.80724ms。第二组有一对稳态
回退1.549%，该对及全部数据保留，没有删异常点或选最快值。
两批共9/12稳态、10/12DRS改善，保留这个范围明确的小收益。

Engine6另做LLPC/ACO control和完整trace。两编译器的20个输出
及各69项读回分别与上轮已保存案例完全一致，traced/untraced全部
输出哈希一致。已有首次非零仍是Engine6 dispatch3 d3d_enc0_attr_f_lt，
attribute9、pixel203,378、byte23234111的0x6e/0x6f FP中点案例。
没有将帧入口历史差异误判为当前dispatch来源。Legacy没有改源码或
SPIR-V，复用已接受的逐位一致控制。

## 保留范围与复现

生产源仅对已测试dec_dfn_96_128_fl_lt启用FC1地址因子。其他核
保持原表达式；不把未测试的公共优化全局启用。上一轮enc96改动保留。
源生成一致性检查必须匹配测试SPIR-V 607b418f7ed5346e、ACO代码
38ac98d35509933f。候选资源、输入及编译策略没有变化。

verify_address_retained_source.py已核对20个相关变体，完整runtime
构建90/90 SPIR-V与测试包逐字节一致。18项CPU测试、Python语法、
PowerShell解析和git diff --check通过。复现辅助脚本显式恢复地址改动
之前的控制源码，保留enc96，避免后续重跑静默叠加新因子。

可执行数据和记录：
build/aco-runtime-trace/experiment-address-screen/dec_dfn_96_128/candidate。
其中已包含上一轮保留enc96。未覆盖staged ASI、原始发布包或已接受
基线。全部后续更改保持未提交。

编译筛选脚本：asi/tests/w2_screen.py、address_screen_build.py。
Windows测试：run_aco_screen.ps1，依次Correctness、Short，再对准备
保留者LongCorrectness、Long。每次使用新Name；汇总用
summarize_aco_screen.py，清理后的文件哈希可从既有归档复用。

相邻证据目录分别保存四个候选results.json、两批长测原始summary、
日志和frame-state CSV、292帧哈希、trace6/hashes.json。详细离线
报告为mesa-aco-w2-screen-2026-11-10.md和mesa-aco-address-screen-2026-11-10.md。

下一项值得解决的问题是高VGPR热点中仍然活跃的矩阵片段和加载依赖，
需要以具体活跃区间及指令为依据提出独立因素；本轮未改变它们。
