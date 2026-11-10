# enc128 有界循环展开研究与组合收尾 — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


保留enc128单个MLP循环的DontUnroll提示及该核的有界cmat地址记录。
独立六对确认相对上一轮保留组合，稳态3.69404→3.66118ms（-0.890%），
DRS3.49100→3.44710ms（-1.258%），两阶段均6/6改善。建议本轮后
阶段性收尾：当前已测试方向的边际收益有限，继续扩大循环搜索的
维护与验证成本较高。收益不构成已证明内存或同步瓶颈的证据。

## 具体循环、触发pass与LLPC对照

目标是onr_block_fast.glsl原866行的MLP切片循环：j从0开始，
j<LT_HID=512，每次增加SLICE=32，实际16次迭代。每次计算32个隐藏
通道的FC1、GELU，再按原K顺序更新FC2累加器。不是DFN/merge循环。

固定Mesa提交96cb43121031992b85767f9c1be8f3f48e22b1d2，
radv_shader.c的radv_optimize_nir迭代调用nir_opt_loop_unroll。
nir_opt_loop_unroll.c:899 check_unrolling_restrictions中，
max_unroll_iterations_aggressive=128，LOOP_UNROLL_LIMIT=26，
cost_limit=3328；本循环pipeline=1、flatten=1、force=0。

在优化已移出首迭代后，待展开部分trip_count=15。临时只读探针记录：

| 地址开关 | 每次估算成本 | 15次成本 | 阈值 | 决定 |
|---|---:|---:|---:|---|
| off | 287 | 4305 | 3328 | 保留循环；后续成本355仍拒绝 |
| on | 221 | 3315 | 3328 | 完全展开剩余15次 |

日志中的trip15是剩余迭代，不把源循环误写成15次。早期的trip15/
body2记录属于内部小循环，不以它作为MLP决策依据。探针还打印具体
循环头SSA，可与循环计数器及FC累加phi核对。探针off/on生成的机器码
分别与已有两格逐字节一致；日志插桩未进入运行记录，Mesa通用
loop-unroll源码已恢复原版。

同SPIR-V的LLPC ELF用本地llvm-objdump-21反汇编。MLP循环起点0x203c，
s10先初始化-32，在0x2170加32，0x2264比较s10>0x1df，在0x257c
回跳0x203c；随之推进FC1/FC2指针。它保留了对应32通道切片循环。
归档ISA中部分gfx12指令由该工具显示为.long，循环计数、地址推进、
WMMA及分支可解码；没有据此声称LLPC内部优化pass与Mesa相同。

## 四格离线对照

复用前轮original/off、original/on。另两格仅给该源循环添加
DontUnroll，未修改全局展开选项或其他循环。spirv-dis逐行diff只出现：

    OpLoopMerge %3938 %3939 None
    OpLoopMerge %3938 %3939 DontUnroll

原SPIR-V FNV5f78c6a07f1a5ec7；限制展开7637570aac01b415。
DontUnroll在final NIR中保留对应的0→512/32计数器和FC累加phi，
与具体MLP代码核对，不以总循环数作为唯一证据。

| 展开策略 / 地址开关 | 入口B | VGPR | SGPR | scratch | LDS B | 普通global load / SGPR基址 |
|---|---:|---:|---:|---:|---:|---:|
| 原策略 / off | 37228 | 240 | 108 | 0 | 21504 | 306 / 130 |
| 原策略 / on | 47692 | 240 | 108 | 0 | 21504 | 816 / 811 |
| 限制该循环 / off | 37228 | 240 | 108 | 0 | 21504 | 306 / 130 |
| 限制该循环 / on | 33108 | 240 | 108 | 0 | 21504 | 306 / 301 |

限制/off机器码逐字节等于原/off，证明loop hint没有引入额外算法
变化。限制/on相对原/off入口减少11.07%；VGPR未降低，仍可入选。

| 静态指令 | 原/off | 原/on | 限制/off | 限制/on |
|---|---:|---:|---:|---:|
| v_add_co_u32_e64 | 267 | 16 | 267 | 16 |
| v_addc_co_u32 | 267 | 16 | 267 | 16 |
| v_lshlrev_b64 | 95 | 10 | 95 | 10 |
| s_add_u64 | 7 | 55 | 7 | 89 |
| s_waitcnt_depctr | 306 | 174 | 306 | 279 |
| s_wait_loadcnt | 190 | 582 | 190 | 227 |

保留循环避免静态load扩张；地址改进减少VGPR宽地址链，同时增加
部分SALU及load等待。调度代价并非全都下降，不根据指令数推断
内存流量或同步瓶颈。保持global/vector8与现有数值策略，原有
浮点表达式和K归约顺序不变；本因素预期不改变输出数值。

没有扩展enc96/dec96循环研究：没有发现或证明同类展开机会。

## GPU筛选、目标核与端到端

Windows D3D12/PAL strict ACO；720p→1080p、相位对齐、flow/direct
guides/output/sampler均1。控制已含前轮enc96与dec96源级优化，
以及dec96 cmat地址记录；候选只加入enc128 loop hint和该核地址记录。
两侧不同data/aco，日志核对实际代码，避免相同SPIR-V共享记录。

先8warmup+8steady+12DRS逐帧正确性，20帧输出全部逐位一致、无非有限。
Fast仍使用既有质量门槛，这里记录相等结果，不设新统一bit-exact要求。
再16warmup+48steady+24DRS两对短测：稳态-1.219%、-1.003%，
中位3.66422→3.62350ms（-1.111%）。DRS中位-1.493%，两对有波动。

独立插桩两对24warmup+640steady+24DRS，各取日志最大596个样本：
目标核0.1214→0.0740ms、0.1213→0.0744ms。约39%的目标核变化只用于
定位；不使用插桩下总耗时作发布性能结论。

升级8warmup+280steady+12DRS长正确性，292帧均逐位一致、无非有限。
随后两组各六对48warmup+240steady+64DRS、轮换顺序，无stamps及
逐帧读回。报告run中位数的中位数，所有轮次均归档：

| 批次 | 稳态控制→候选ms | 变化 / 改善 | DRS控制→候选ms | 变化 / 改善 |
|---|---:|---:|---:|---:|
| 第一组 | 3.82020→3.72880 | -2.393% / 6/6 | 3.55812→3.45648 | -2.857% / 6/6 |
| 无并行构建的独立确认 | 3.69404→3.66118 | -0.890% / 6/6 | 3.49100→3.44710 | -1.258% / 6/6 |

第一组期间有CPU构建/归档任务，绝对值和波动较大，控制范围
3.71756–3.84184、候选3.66024–3.80096ms，不能把2.393%当作稳定
增益。保留该组全部原始数据；主要采用无并行构建的确认批次评估：
控制3.68184–3.70528、候选3.64860–3.67396ms，逐对稳态改善
0.594%–1.464%。没有删除不利轮次或选最快值。

## 准备纳入构建的精确范围

1. enc_stage_96_128_128_fl_lt：既有单tile及保留W2 reciprocal政策，
   SPIR-Vf3817f3fb0a26545，代码c08996618a61cd08。
2. dec_dfn_96_128_fl_lt：既有FC1 row/tile整数地址源改动，
   SPIR-V607b418f7ed5346e；本轮组合使用cmat地址记录6a4ccda0cd2efb14。
3. enc_stage_128_128_160_fl_lt：仅该变体定义MLP_SLICE_LOOP=
   DontUnroll，SPIR-V7637570aac01b415，cmat地址代码591c303d1916df59。
4. Mesa capturev4追加独立global-address.patch，默认不启用；记录
   生成器清理继承的ONR_ACO_CMAT_ADDRESS，只对build_asi.sh显式传入
   的上述dec96和enc128精确stem启用。stem列表进入fingerprint及
   manifest，toolchain记录新增patchhash。其他核保留原lowering。

没有新增运行时编译器分支或LLPC回退。保持pinned版本、global/vector8。
原始发布包未覆盖，源码和构建脚本保持未提交。

完整重建隔离组合包：build/aco-runtime-trace/retained-combination/data。
当前源码生成90个SPIR-V、按正式选择生成90个记录，全部逐字节匹配
此前测试组合；证据combination-build.json。临时unroll探针不在构建中。

必要组合验证已经完成：Fast20逐帧质量通过，Legacy20逐位一致；
LLPC/ACO两侧各20个输出和69个Engine6读回逐字节复现既有案例，
traced/untraced一致。既有首次非零仍在Engine6 dispatch3 enc0_attr，
没有新误差源。292帧比较覆盖所有保留项组合相对此前保留版本。

另直接测量完整组合相对原始ACO，六对48/240/64：稳态
3.76560→3.66970ms（-2.547%），DRS3.54452→3.47048ms（-2.089%），
两阶段均6/6改善。控制/候选稳态范围3.75564–3.78788/
3.65984–3.68528ms。该值是同批组合实测，不能用各轮百分比相加替代。
本轮未重跑已接受LLPC性能表，也不宣称补齐此前约19%差距。

## 维护成本、建议与复现

维护成本集中在版本固定的Mesa附加补丁、两个精确记录选择与一个
局部loop hint；以后升级Mesa需重新核对安全匹配、unroll阈值及
三项代码身份。收益足以保留当前组合，暂不值得扩大成多核调参
系统或更改全局展开规则。建议阶段性收尾，交付审查后再决定更大
结构方向；结论为“当前已测试方向的边际收益有限”，不是架构上
没有进一步空间，也不是已证明瓶颈归因。

离线：enc128_unroll_screen.py复用原两格并补齐局部DontUnroll；
prepare_enc128_loop_probe.py及capture_enc128_loop_probe.py记录判据；
探针必须在离线研究后恢复，不能作为正式编译器。正式流程为
mesa/build_capture.sh，build_shaders.py，build_aco_records.py
--precision global --matrix-loads vector8 --cmat-address-shaders
dec_dfn_96_128_fl_lt enc_stage_128_128_160_fl_lt。完整插件构建脚本
已接入相同选择；本轮没有运行覆盖原发布目录的整包构建。

相邻compiler-evidence.zip保留四格NIR/IR、成本探针日志、源diff及
SPIR-V，archive.json逐成员哈希；enc128-llpc.isa保留同SPIR-V循环
对照。profile.json保存目标核定位数据。GPU原始summary、日志、
frame-state及292帧哈希位于next-optimization证据目录的
enc128-unroll、enc128-unroll-confirm及combination-final。
组合正确性证据在相邻combination-correctness/。

最终retained-combination.zip归档194项源码、构建配置、SPIR-V与记录，
retained-combination.json逐文件校验；combination-record-checks.json
核对18个长测组合上下文的三项实际代码身份。清理618个可再生读回，
回收10,251,878,400B（9.55GiB），保留完整组合Engine6 trace和长序列
steady131/279、DRS11，全部日志/summary/CSV/输出哈希保留。清理后
C盘空闲约61.85GiB，清单在next-optimization/cleanup-unroll-*.json。
18项CPU测试、Python语法、PowerShell解析、bash -n、git diff --check
通过。未提交、未合并、未覆盖原始发布包。
