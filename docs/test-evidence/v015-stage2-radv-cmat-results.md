# Stage 2 RADV coopmat 访存降低：cmat-address 全核扩面 + shared 向量化三档（档位 I 胜出）

分支 `codex/mesa-263-comparison`（`ebda333` §7 勘误之上）。被测 = 档位 I 产物（cmat-address 扩至 21 核，shared-cmat 关闭）。

## 0. 改动

| 项 | 内容 |
|---|---|
| `prepare_global_address.py` | 通用化至 `elem_bytes = bits/8`（1=FP8, 2=FP16），`ptr_stride == elem_bytes` 守卫，`30u/30u` 溢出界不变（worst: `15·stride·2+15·2 ≤ 30·stride+30`） |
| `prepare_capture.py` | 新增 `ONR_ACO_SHARED_CMAT=load/loadstore`：shared coopmat 同 lane 连续 run 向量化（load: 向量 `nir_load_deref`；store: `nir_vec` + `BITFIELD_MASK` 一次 `nir_store_deref`）；严格保留 Mesa 原 `align_mul/align_offset` 标注 |
| `build_aco_records.py` | `--shared-cmat` CLI + `policy` 指纹字段 |
| `build_asi.sh` | `--cmat-address-shaders` 从 2 核扩至 **21 核**（`OPENRR_ACO_SHARED_CMAT` 默认 off）；`--shared-cmat` 管道接入 |
| Mesa 工具链 | 补丁重打（`capture.patch`/`global-address.patch` 指纹已更新，toolchain.json 已刷新） |

## 1. 档位 I 扩面选择依据（静态）

| 家族 | exec 变化 | 决定 |
|---|---|---|
| 160 家族（4 对 _f/_fl） | **−1,320 ~ −2,524 B** | ✓ 加入 |
| 96 家族（enc/dec_up/dec_dfn _f/_fl） | −284 ~ −1,432 B | ✓ 加入 |
| W1/W2（enc0_stage、dec_head、dec_up_32、enc_stage_64、dec_dfn_64） | −808 ~ −1,012 B（dec_dfn_64 +544, dec_up_64 +1,524 除外） | ✓ 加入（除 dec_dfn_64、dec_up_64） |
| 128 家族 _f（dec_dfn/dec_up/enc_stage） | **+13,956 ~ +14,092 B（+55%！）** | ✗ 剔除（无 `MLP_SLICE_LOOP dont_unroll`，越全展开成本上限） |
| 128 家族 _fl | enc_stage_128_fl 已有 ✓；dec_dfn_128_fl/dec_up_128_fl +13,940/+14,028 | ✗ 剔除 |
| **意外收获** | `dec_up_32_64_64_f_lt` VGPR **256→216**（跨入 7 waves）；`enc_stage_64_64_96_f_lt` 256→240 | |

## 2. 三档实测（均 3×32/120/64 ABBA，vs 同栈父构建，全部 0 ULP）

| 档位 | 1080p | 4K | 0 ULP |
|---|---:|---:|---|
| **I**（cmat-address 21 核） | **−2.80%** | **−1.63%** | exact 100% |
| II（+ shared load 向量化） | −2.56% | −1.21% | exact 100% |
| III（+ shared loadstore） | −2.47% | −2.27% | exact 100% |

三档互差 ≤0.4%（噪声带）——**shared 向量化在已扩面的 cmat-address 基础上无额外净收益**（WSTEP/PV_T 的 LDS 访存已由 cmat-address 的有界字节偏移覆盖了大部分收益面）。按最简原则保档位 I。

## 3. Step 4 门禁与最终测速

| 门禁 | 结果 |
|---|---|
| test_asi_units | ALL PASSED |
| Legacy 0 ULP（vs 冻结基线） | exact 100.0000%，ulp 0 |
| g1 ACO-vs-ACO(父) | **999 dB（逐位一致）** |
| Fast-ACO vs 父构建 | 0 ULP（档位 I 门禁已过） |

**最终 3×48/240/64 vs 同版 Fast-LLPC**：

| 档 | ACO strict | Fast-LLPC | 差距 | 此前（`3b23cef`） |
|---|---:|---:|---:|---:|
| 1080p | 3.239 ms | 3.013 ms | **+7.49%** | +11.61% |
| 1440p | 5.492 ms | 5.154 ms | **+6.55%** | +10.08% |
| 4K | 13.611 ms | 12.922 ms | **+5.33%** | +8.29% |

→ **LLPC 差距收窄 4.1/3.5/3.0 个百分点**（1080p/1440p/4K）。门禁线（3.009/11.389 ms）仍未跨过；剩余差距主体仍在 128 家族（本次被代码爆炸剔除的三对 _f/_fl + LDS 上限封顶效应）。

## 4. 结论与后续

1. **档位 I 保留**（cmat-address 21 核扩面，shared-cmat 关）：0 ULP、−2.80/−1.63% 加速、LLPC 差距 +8.3→+5.3% @4K。
2. 128 家族是下一个目标：需要先给 `dec_dfn_128/dec_up_128` 的 `.comp` 加 `MLP_SLICE_LOOP [[dont_unroll]]`（同 `enc_stage_128_128_160.comp:13-15` 的先例）才能安全过 cmat-address；或者从 LDS 端（32768→削 tile 布局）破 4 waves 上限。
3. shared 向量化（II/III）数据已留档，后续如 LDS 布局变化使 WSTEP/PV_T 重新成为瓶颈时可重开。

## 5. 复现

```bash
# 档位 I（ship 默认）
bash asi/build/build_asi.sh build/asi-015-aco
# 档位 II/III
OPENRR_ACO_SHARED_CMAT=load|loadstore bash asi/build/build_asi.sh build/asi-015-aco
# 门禁/测速同 §2-3 口径
```
