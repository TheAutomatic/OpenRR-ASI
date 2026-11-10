# Same-SPIR-V hotspot resources — 2026-11-10

Compile-only Windows LLPC driver ELF versus pinned ACO global/vector8 records. Each row pairs the exact SPIR-V hash; retained enc96 is a separate row. No GPU dispatch or timing in this capture.

| Shader / variant | SPIR-V FNV | VGPR LLPC/ACO | Scratch B/thread LLPC/ACO | LDS B/group LLPC/ACO | Entry bytes LLPC/ACO |
|---|---|---:|---:|---:|---:|
| dec_dfn_96_128_fl_lt.spv / original | 9b09c8e4e1c7b892 | 232/256 | 0/0 | 9728/9728 | 20580/27560 |
| enc_stage_128_128_160_fl_lt.spv / original | 5f78c6a07f1a5ec7 | 223/240 | 0/0 | 21504/21504 | 16024/37228 |
| enc_stage_96_128_128_fl_lt.spv / original | 581ef3189b0a0052 | 246/256 | 0/8 | 12288/12288 | 21428/39544 |
| dec_up_160_256_160_fl_lt.spv / original | 360f7a5dd195af7c | 170/192 | 0/0 | 30208/30208 | 17596/24776 |
| enc0_stage_32_64_f_lt.spv / original | 0705fad577a0f8a7 | 191/192 | 0/0 | 0/0 | 21412/25392 |
| dec_head_32_64_f_lt.spv / original | 3eccd9693a0d87ef | 182/192 | 0/0 | 0/0 | 19396/23564 |
| dec_up_128_128_160_fl_lt.spv / original | 5d05fcc3de384276 | 230/256 | 0/0 | 32768/32768 | 13080/28324 |
| dec_dfn_128_128_fl_lt.spv / original | 4ecf3b5cd58f6a11 | 216/256 | 0/0 | 26112/26112 | 14412/29236 |
| dec_dfn_160_256_fl_lt.spv / original | 6bc90aef575d98cf | 172/192 | 0/0 | 30208/30208 | 19068/24468 |
| enc_stage_96_128_128_fl_lt.spv / retained | f3817f3fb0a26545 | 201/240 | 0/0 | 19456/19456 | 13752/38028 |
| dec_dfn_96_128_fl_lt.spv / address-retained | 607b418f7ed5346e | 232/256 | 0/0 | 9728/9728 | 20580/26488 |

LLPC entry size is the _amdgpu_cs_main ELF symbol size; ACO entry size is capture exec_bytes before the PAL adapter prologue. ELF/.text sizes and SGPR metadata are in JSON; .text padding, non-code ELF sections and the adapter are not counted as shader body. These are compiler-export metadata, not a direct observation of final Windows execution registers.

LLPC exported ELF and device provenance are saved in hotspot-llpc-elf.zip; per-row ELF SHA256 and exact SPIR-V SHA256 are in JSON. ACO code identity is bound to its manifest and record.
