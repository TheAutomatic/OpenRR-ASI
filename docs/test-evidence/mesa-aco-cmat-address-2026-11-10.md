# Cooperative-matrix global lane 地址实验 — 2026-11-10

<!-- aco-delivery-archive-entry -->
> 交付整理：本文引用的一次性实验脚本已归档。先在仓库根运行 verify: verified 59 explicitly listed tools，再运行 restore: verified 59 explicitly listed tools 恢复原路径，然后执行本文历史命令。完整清单、归档与当前组合复现入口见 [交付说明](mesa-aco-delivery-2026-11-10.md)。恢复脚本不会恢复已清理的大文件；先核对相应证据归档。


本轮检验公共地址生成假设，不修改GLSL、权重、精度规则或矩阵运算。
独立环境开关 ONR_ACO_CMAT_ADDRESS=1，只作用于有静态范围证明的
global column-major FP16 vector8 load。enc128发生额外展开、代码增大，
离线排除；enc96短测不稳定，暂缓；dec96短测有改善，进入长验证。
最终保留决定以末节结果为准。

## Pinned pass顺序与具体断点

固定Mesa96cb43121031992b85767f9c1be8f3f48e22b1d2，capturev4、
global/vector8、wave32。私有Mesa源码位于WSL
/home/kenny/work/openrr-mesa-aco/source。没有更换编译器版本。

1. radv_shader.c:619先lower flexible dimensions，626调用
   radv_nir_lower_cooperative_matrix；此时仍是deref。
2. radv_nir_lower_cooperative_matrix.c:281起处理load/store，A输入先翻转
   layout；GFX12 row-major transpose load在前面的独立分支退出。
   本次匹配的是翻转后column-major、普通load的后续分支。
3. 该分支inner_idx=lane&15，wave32的base_row=(lane/16)*8。
   col_offset先32位乘stride，再u2u64；row_offset单独u2u64。
   随后建立ptr_as_array(col_offset)、cast、ptr_as_array(row_offset)。
   现有vector8补丁在此将连续8个FP16元素合并加载。
4. radv_shader.c:875调用nir_lower_explicit_io global/constant，地址格式
   64bit_global。nir_lower_explicit_io.c:1245仅对in-bounds普通array
   使用窄索引乘法；ptr_as_array走1248的i2iN及宽乘法。原始shader
   索引可能有符号，不能因为lane偏移非负就改掉原索引。
5. radv_pipeline.c:548，ABI lowering、SMEM flag、访问位宽调整及late
   vectorize之后才调用ac_nir_lower_global_access，随后nir_lower_int64。
   global pass的try_extract_additions可剥常量、u2u64和iadd；对u2u64
   内部合并使用is_nuw/范围分析。穿过imul/ishl时mul!=1不能直接提取
   一般动态out_offset；乘法要求nuw时也不能凭空假定无溢出。

enc128归档NIR3802–3808对应的链包含有符号源offset扩展、两次64位
向量iadd、64位ishl、再加uniform pointer；最终global load使用完整
VGPR地址和undef标量基址。具体不是“标量基址丢失”，而是多层deref
在explicit-IO后形成的带缩放64位表达式无法被晚期匹配器完整拆开。
本轮不扩大晚期全局匹配规则，先在cmat仍有lane/stride依据时保持形态。

## 单因素候选与整数证明

prepare_global_address.py叠加在capturev4之上，生成global-address.patch。
条件同时要求：is_load、GFX>=11.7、wave32、column-major、mul=1、
length=8、FP16、ptr_stride=2、global deref、常量stride和vector8开启。
不覆盖store、LDS、transpose load、动态stride、其他元素或布局。

保留原始64位deref，包括shader起始索引的符号扩展及64位缩放。仅把
cmat自行生成的lane偏移合并为32位字节offset：

    column = (lane & 15) * stride * 2
    row = ((lane / 16) * 8 + row_iter) * 2
    byte_offset = column + row
    address = original_64bit_pointer + zero_extend(byte_offset)

lane∈[0,31]，row_iter∈[0,7]，因此列号≤15、行号≤15。仅当
stride≤floor((UINT32_MAX-30)/30)=143165575时启用。它证明原来的
32位column*stride无溢出、新的乘2及最终加法都不超过UINT32_MAX。
不存在把原有有符号索引无条件转uint32。负/过大stride的uint表示
不能通过此界限。base保持64位，跨4GiB carry由global硬件寻址承担。

新偏移仍依赖lane，不使用readfirstlane、SMEM强制scalarization或
divergence标记覆盖。设置nuw只用于已证明范围的column+row。
FP运算、加载数据顺序、FMA/reciprocal和FP8/16/32边界均不变；预期
无数值变化。Fast继续使用现有阈值，不设置统一bit-exact门槛。

CPU精确整数模型163072组，含87022次跨4GiB进位、原始signed32
索引极值、64位base wrap、stride极值及32lane检查全部通过。
这是模型验证，不是GPU人为布置跨4GiB分配；runtime验证使用真实
Windows分配地址。stride8会产生合法的重叠行地址，因此lane唯一性
仅在不重叠stride16/32/128上断言，其余仍逐lane验证地址等价。

## 相同SPIR-V离线结果

控制为上轮两个保留优化都已在内的当前数据包：
experiment-address-screen/dec_dfn_96_128/candidate/data。
三个off机器码均逐字节匹配已接受归档，证明关闭开关不改变基线。

| 核 / SPIR-V FNV | 代码B off→on | VGPR | SGPR | scratch B/thread | LDS B/group |
|---|---:|---:|---:|---:|---:|
| enc128 / 5f78c6a07f1a5ec7 | 37228→47692 | 240→240 | 108→108 | 0→0 | 21504→21504 |
| retained enc96 / f3817f3fb0a26545 | 38028→36084 | 240→240 | 108→108 | 0→0 | 19456→19456 |
| retained dec96 / 607b418f7ed5346e | 26488→24372 | 256→256 | 108→108 | 0→0 | 9728→9728 |

代码体积为ACOexec_bytes，不含PAL适配前导码；SGPR为捕获导出值。

| 静态指令 / 形态 | enc128 off→on | enc96 off→on | dec96 off→on |
|---|---:|---:|---:|
| v_add_co_u32_e64 | 267→16 | 143→13 | 167→26 |
| v_addc_co_u32 | 267→16 | 143→13 | 167→26 |
| v_lshlrev_b64 | 95→10 | 51→8 | 67→19 |
| s_add_u64 | 7→55 | 5→45 | 31→77 |
| s_lshl_b64 | 7→55 | 5→45 | 7→53 |
| s_waitcnt_depctr | 306→174 | 334→175 | 284→279 |
| s_wait_loadcnt | 190→582 | 298→317 | 92→91 |
| 使用SGPR基址的普通global load | 130→811 | 96→515 | 26→91 |
| 普通global load总数 | 306→816 | 519→519 | 98→98 |

向量64位运算和部分依赖等待确实减少，标量64位运算增加；不能将
所有wait或所有64位成本都说成减少。enc128 final NIR少一个con loop，
静态global_load_dwordx4由175增至685，说明优化还触发额外展开。
未据静态load数量推断动态流量增加；它解释静态代码增大的来源之一。
因此enc128不送GPU。enc96/dec96的load数量和NIR循环数均不变。

## GPU测试隔离与记录身份

这些候选SPIR-V与控制相同，改变的是代码记录。原有共享ONR_ACO_DIR
不能区分两侧相同hash的记录；本次新增-PerDataRecords，让runtime
按各自data/aco读取，清理ONR_ACO_DIR。日志确认enc96控制代码
c08996618a61cd08、候选1ba94d9e00ca5ad3；dec96控制38ac98d35509933f、
候选6a4ccda0cd2efb14。只替换目标记录，其他当前记录保持一致。
不能用两侧都加载候选记录的比较声称收益。

同一Windowsopenrr-trace.asi，严格全ACO、direct guides/output、sampler、
flow均1；相位对齐720p→1080p。正确性8warmup+8steady+12DRS，
两候选20帧输出均逐位一致，无非有限。short为16warmup+48steady+
24DRS，两对轮换顺序，无逐帧读回或stamps。角色按order.json归一化。

| 候选 | 稳态中位ms 控制→候选 | 逐对变化 | DRS中位变化 | 决定 |
|---|---:|---|---:|---|
| enc96 | 3.71604→3.71394 | +1.246%、-1.335% | -0.659% | 暂缓 |
| dec96 | 3.73382→3.70690 | -0.078%、-1.366% | -1.288% | 进入长验证 |

enc96中位仅-0.057%，不足以升级；dec96两对DRS也都改善，但两对
短测不能代表稳定收益，因此须长序列正确性和重复端到端测量。

## 复现与证据

离线补丁是独立文件，不修改capture.patch和global/vector8策略。
构建前原库及cmat源备份于build/aco-tools/global-address-baseline。
原库SHA256a12416274af850a9d5031bba86574231b8fadb7ef5bf0be4e0170544d3cba36c；
实验库a50ea161e71bbe283c187a01aef41da2f7052f5f027ed6bcf62349a7c59b4f58。
实验配置独立保存experiment-cmat-address/toolchain.json，记录patchhash。
候选各自manifest内experiment字段保留此编译器身份，基础策略不变。

WSL离线：在新应用capturev4的私有源码上运行prepare_global_address.py，
ninja构建；cmat_address_screen.py分别设置/清理开关，编译三个相同
SPIR-V；cmat_address_boundaries.py；prepare_cmat_address_candidates.py；
summarize_cmat_address.py归档。不要反复对已打补丁源码运行preparer。

Windows：run_aco_screen.ps1 -Name新名 -Candidate对应candidate目录
-Baseline当前retained data -PerDataRecords -Stage Correctness，检查
实际记录代码身份再Short。汇总使用summarize_aco_screen.py。

相邻screen.json保存资源、64位NIR统计、opcode和标量基址示例，
compiler-evidence.zip包含离线IR、patch及工具链，archive.json记录
成员哈希。GPU原始summary、日志、frame-state和输出哈希在
mesa-aco-next-optimization-2026-11-10/cmat-{enc96,dec96}/。
未提交、未合并，未覆盖发布包。

## 长验证与本轮决定

dec96完成8warmup+280steady+12DRS，292帧输出均与当前控制逐位一致，
无非有限。另跑Engine6 trace：双方各69项读回均逐字节匹配已保存的
当前ACO案例；20个traced/untraced输出也一致。因而原LLPC/ACO首次
非零案例保持原样，没有引入新的首次分歧。Legacy所用记录不变，
没有将Fast20帧结果推广成对新编译器全布局的Legacy保证。

两组六对48warmup+240steady+64DRS长测，均轮换先后顺序，计时段
不做逐帧读回、不启用stamps。中位数是六个run中位数的中位数：

| 批次 | 稳态控制→候选ms | 变化 / 改善对数 | DRS控制→候选ms | 变化 / 改善对数 |
|---|---:|---:|---:|---:|
| 第一组 | 3.72974→3.71226 | -0.469% / 6/6 | 3.52772→3.52432 | -0.096% / 3/6 |
| 独立确认 | 3.72734→3.71430 | -0.350% / 5/6 | 3.52262→3.48920 | -0.949% / 5/6 |

第一组稳态控制范围3.72368–3.74176、候选3.69748–3.71964ms；
确认组控制3.70016–3.74200、候选3.71120–3.73100ms。第一组配对
改善-0.218%至-0.740%；确认组五对改善-0.221%至-0.444%，一对
回退+0.298%。全部轮次保留；没有用单次最快值或排除回退轮次。
DRS总体波动更大，不能把第一组接近持平描述成稳定DRS收益。

保留dec96的离线编译候选及记录，作为后续公共地址生成研究的已测
参考。既有生产GLSL与默认global/vector8记录不改；独立candidate目录
仅替换dec96记录，已包含此前保留enc96与dec96源级优化。没有全局
启用新lowering。enc96暂缓、enc128离线排除，保留其失败证据。

实验库已隔离到experiment-cmat-address/compiler，独立ICD和
isolated-toolchain.json可重放；默认capture库及cmat源码已恢复原
capturev4，库SHA256再次核对与toolchain.json一致。

清理本轮658个可再生读回，回收10,915,430,400B（10.17GiB）；保留
完整Engine6 trace及长序列steady131/279、DRS11，日志、summary、CSV
及292帧哈希全部保留。清单在next-optimization证据目录的
cleanup-cmat-files.json与cleanup-cmat-summary.json。清理时C盘空闲
约67.12GiB。后续可直接根据记录和输入重新生成其他读回。

18项CPU测试、Python语法、PowerShell解析及git diff --check通过。
verify_cmat_gpu_records.py核实40个角色/代码身份，包含两批长测及trace。
下一步若扩展这个因素，应先控制enc128的展开变化，再独立比较；
不能依据本轮dec96结果扩大到store、LDS或未验证布局，也没有证明
该地址问题解释全部代码膨胀或剩余LLPC性能差距。
