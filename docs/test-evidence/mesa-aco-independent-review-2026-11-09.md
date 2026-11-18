# Mesa ACO 独立审查 — 2026-11-09

审查对象：codex/mesa-aco 的未提交实现。未改实现或记录；独立测试使用交接中的 ASI，SHA256 为 BE66500153EC0EF6403709C0A3A9B5DA1EF8616821D9EA12F8F72FAB9005797A。

## 结论

当前 Fast 性能回退可以复现，但尚未证实由 PAL 接线错误导致。当前候选包含全局 exact 和实验性矩阵加载修改，不能将结果概括为原生 ACO 相对 LLPC 的能力。优先拆开编译器改动、补齐实际发布路径的诊断，再继续算子优化。

## 审查发现

### 1. [P1] 全局 exact 改变了整个程序的浮点优化约束

asi/build/mesa/prepare_capture.py:70 对所有 ALU 无条件 OR nir_op_valid_fp_math_ctrl(op, nir_fp_exact)。固定 Mesa 源码 nir.h:1596 定义 exact 为 no_contract | no_reassoc | no_transform，影响范围超出显式 FMA 和半精度往返转换，Fast 核也受影响。

这是确定的编译配置差异，是性能嫌疑，尚未通过单变量实验测得其贡献。先分离“记录导出/ABI 必需补丁”“特定精度修复”“矩阵加载实验”三个层次。保留显式 FMA 与确实依赖舍入的位置约束，对全局 exact 与局部约束生成独立记录集，以相同 SPIR-V/同一加载器对比结果和耗时。旧 bench-minexact 不是当前完整快照的单变量证据，不能直接归因。

### 2. [P1] 地址高位重写没有证明修改对象确实是地址常量

asi/build/build_aco_records.py:79-94 把普通 s_movk_i32 0x8000/-32768 视为候选，向后最多查 40 条反汇编指令寻找 s_load/global_load 的寄存器对。它不验证全部用途或控制流，也不验证解析流覆盖全部指令字节。后续一次指针使用不能证明同一常量的其他用途可以一起改变。

现有参考实现已提供更直接的方案：
C:/Users/Kenny/Downloads/neural-amd-opti-reference/tools/aco-records/mesa-25.3.6-nr-record.patch
C:/Users/Kenny/Downloads/neural-amd-opti-reference/tools/aco-records/make_record.py 的 m0_code()

该实现设置地址专用哨兵、在 ACO assembler 按操作数替换 m0，并比较两个不同哨兵的捕获结果是否相同，同时统计 m0 写入和常量数据。应将其移植到固定 Mesa 版本，用编译器结构完成适配。当前没有证据把这一缺口认定为 19% 回退的原因。

### 3. [P2] 逐核 profile 与差异诊断没有覆盖发布 Fast 路径

asi/tests/aco_compare.hpp:36-60 用 Engine::kernel_names(..., true) 和普通 load() 加载基础 _f 核，随后关闭 flow 和 history sampler；运行时则由 runtime/src/runtime.cpp:595 加载 _lt，并使用 flow 变体和历史采样。旧 profile 中 mode=Fast 因而不代表发布 Fast。

此外，两个核集加载完成后 D3dDevice 保持 ACO，profile 循环未按 c 切换设备的内部编译器状态；内部 fill 仍可能使用 ACO clear_range，即使当前标签写 LLPC。已修正的 set_compiler() 并不会自动修正这个调用方。

补上当前发布核集、lite/flow/sampler 配置、内部清零编译器的一致性，并将“基础核隔离 profile”保留为单独模式。对 Fast 差异逐层找首次分歧，覆盖稳态和 DRS 的每个被测输出，不只比较阶段最后一帧。

## 已核对的接入部分

- ACO 加载路径确实导入 ACO 记录，LLPC 对照显式选择；两组日志均确认 D3D12 和 Fast ready。
- push 的 244B 容量、entry 4 起点、entry 65 描述符指针在加载/绑定端一致。
- 标准采样表有独立的 64B stride、sampler offset 48，并按录制 slot 分配。
- wave32 VGPR 编码 ((rsrc1 & 63) + 1) * 8 与固定 Mesa 的编码公式一致。
- 独立运行已有 CPU 测试：400 个前导码模拟、10 类坏记录及 129 个当前记录全部通过。
- 单纯将 sgpr_count=108 视作占用率异常没有依据；不能据此直接改小。

资源样例（当前 ACO manifest / 已保存 LLPC ELF，不代表完整最终 pipeline 硬件观测）：

| 核 | LLPC VGPR | ACO VGPR | LLPC/ACO LDS | LLPC/ACO scratch 每线程 |
|---|---:|---:|---:|---:|
| enc_stage_128_128_160_f | 223 | 240 | 21504 / 21504 | 0 / 0 |
| enc_stage_96_128_128_f | 246 | 256 | 12288 / 12288 | 0 / 8 |
| dec_up_128_128_160_f | 232 | 256 | 32768 / 32768 | 0 / 0 |

这些差异已存在于捕获记录，不能全部解释为 PAL 导入时增加的资源开销。需同时核查代码生成和导入配置。

## PAL 元数据待核对项

所有 ACO 核都使用 clear_range 容器。rr_pal_binary.hpp:359-374 替换了主要资源字段，但 .image_op 等仍继承模板；记录的 rsrc3 仅被解析校验，没有直接映射到 PAL 元数据。固定 Mesa radv_shader.c:2471 用 rsrc3 表达 gfx12 instruction prefetch，当前记录该值普遍为 2048。

这说明模板到候选的元数据对应关系还未闭合，但不能直接判定 prefetch 在最终 PAL 管线中为零或 .image_op 一定导致回退。下一步应依据 PAL schema/驱动建管线规则补出字段对应表，再用相同机器码、只改变已确认字段的实验验证。不要直接复制 Linux 原始寄存器位或猜测字段值。

## 独立 GPU 对照

同一 ASI、同一 SPIR-V 和记录，1280x720 -> 1920x1080，Fast；预热 24 帧、稳态 48 帧、DRS 24 帧。只改变 flow 与执行顺序。日志确认 flow=0 使用 barriers，flow=1 使用 row flags。

| flow | 顺序 | LLPC 稳态 ms | ACO 稳态 ms | ACO 增幅 | LLPC DRS ms | ACO DRS ms |
|---|---|---:|---:|---:|---:|---:|
| 0 | LLPC -> ACO | 3.15956 | 3.76368 | 19.1% | 3.10400 | 3.61840 |
| 0 | ACO -> LLPC | 3.15256 | 3.75908 | 19.2% | 3.06492 | 3.62464 |
| 1 | LLPC -> ACO | 3.14192 | 3.73892 | 19.0% | 3.05332 | 3.63992 |
| 1 | ACO -> LLPC | 3.14220 | 3.71188 | 18.1% | 3.01356 | 3.56976 |

四组均通过现有 Fast 阈值，无非有限值。稳态 PSNR 59.7444 dB、max 54 ULP；DRS PSNR 63.0321 dB、max 39 ULP，四组相同。说明当前差异可稳定复现，不是 flow 开关或测试顺序造成。先前短测的 DRS 100% 一致仅是所比较末帧的结果，不能推广到 DRS 全过程。

新证据位于同工作树 build/aco-review-independent/ 下的四个目录，另有 build/aco-review-flow*.log。本轮未重编译、未更改源码、未提交。

## 建议执行顺序

1. 修正诊断核集与内部清零选择，定位实际 _lt 路径首次差异及热点。
2. 拆分全局 exact 与矩阵加载补丁，做独立记录集的单变量对照。
3. 复用源级哨兵地址适配，替换反汇编启发式。
4. 补齐 PAL 元数据字段映射并验证同代码不同配置；最后才继续改算子。

Mochi 的成功证明方法可行，但其 RDNA4 gemm1x1 使用 descriptor-backed storage buffers 和不同矩阵加载形态，OpenRR 大量使用 BDA；编译路径、寄存器压力和精度规则并不相同。当前证据不足以保证 ACO 必然更快，也不足以宣告方案无效。
