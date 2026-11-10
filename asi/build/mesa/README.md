# Offline ACO for OpenRR D3D12

This path targets gfx1201 (RX 9070 series), wave32 and the
openrr-d3d12-v1 ABI. The normal Windows Vulkan driver still supplies a PAL ELF
container. The adapter replaces its entry, code and compute resource metadata
with the offline ACO record. Vulkan execution uses its existing compiler.

## Source and build

Mesa 26.2.4 is pinned to commit
96cb43121031992b85767f9c1be8f3f48e22b1d2. The installed Ubuntu Mesa 26.0.8
package is not used for capture. There is no source downgrade or network fetch.
The source tree must exist at $HOME/src/mesa, or OPENRR_MESA_SOURCE. The build
archives that commit into $HOME/work/openrr-mesa-aco/source, or
OPENRR_MESA_WORK/source, before applying the patch. The original source is kept
intact. Linux build dependencies include Meson, Ninja, a C/C++ compiler,
Vulkan development headers and Mesa's RADV/drm-shim dependencies.
This machine also uses $HOME/src/mesa-deps/lib/x86_64-linux-gnu for dependencies.

From the repository directory under WSL:

    bash asi/build/mesa/build_capture.sh
    python3 asi/build/build_aco_records.py --out build/asi/stage/openrr-amd/aco \
        --precision global --matrix-loads vector8 \
        build/asi/stage/openrr-amd/shaders build/asi/occ_nopad \
        build/asi/occ_pad16k build/asi/b2_*

For the complete Windows package, use OPENRR_BUILD_ACO=1 with
asi/build/build_asi.sh. BUILD_LOCAL_ASI.cmd forwards that setting to WSL.
Only the record files and manifests are required at runtime;
Mesa, the capture executable and capture/ diagnostics are build tools.
The toolchain fingerprint includes capture executable, script, toolchain
configuration, RADV library hash and capture patch hash. Unchanged inputs reuse
records. New FP8 SPIR-V can enter the same command, but must pass its own GPU
validation before release.

## Capture patch version 4 and controlled review records

Version 4 replaces disassembly-based address32 rewriting with source-level ACO
operand substitution, following the OpenNR record producer in
neural-amd-opti-reference/tools/aco-records/mesa-25.3.6-nr-record.patch and
make_record.py. Mesa 26.2.4 receives the address sentinel through radv_device.c's
compiler-info hardware block. Two separate processes compile the same SPIR-V
with 0x13579bdf and 0x2468ace1; their code, resources, argument mappings and
descriptor layouts must agree. Assembler reports reject m0 writes, constant
data and PC-relative body instructions. Per-program counters avoid shared
mutable assembler state. llvm-objdump is no longer used to create records.

--precision native retains Mesa's original floating rules; local strengthens
weak FMA and marks floating conversions exact; global adds exact controls to
every supported ALU. --matrix-loads scalar/vector8 separately controls the
experimental matrix load patch. The options participate in the fingerprint and
manifest. "local" names a candidate policy, not a claim that its precision has
already passed validation.

The complete package build explicitly retains global/vector8 while review is
pending. OPENRR_ACO_PRECISION and OPENRR_ACO_MATRIX_LOADS override those choices.
The direct record tool defaults to local/scalar, so pass options explicitly.
capture_review_matrix.sh writes five isolated record roots under
build/aco-review-v4; compare_review_records.py compares them entirely on CPU.
Existing v3 package records and the original handoff ASI remain available.

## Capture implementation

prepare_capture.py generates capture.patch against the pinned source. It hooks
radv_shader_nir_to_asm after radv_postprocess_binary_config to export actual
ACO machine code, resource configuration, scratch bytes per thread, wave size,
workgroup size and RADV user SGPR allocations. Descriptor layout export lives
in radv_descriptor_set.c. aco_capture.cpp uses gpu/src/vk.cpp Device::init and
Device::load rather than duplicating the runtime layouts or feature selection.

Mesa 26.2 uses the AMD noop DRM shim for offline gfx1201 enumeration:
LD_PRELOAD=libamdgpu_noop_drm_shim.so, AMDGPU_GPU_ID=gfx1201 and the private
radeon_devenv ICD. RADV_FORCE_FAMILY is not the mechanism used here.

The patch also corrects divergent load_push_constant lowering in
nir/radv_nir_lower_descriptors.c. SMEM implicitly scalarized lane-varying
offsets through readfirstlane. The patch uses a read-only global vector load
for divergent offsets; uniform offsets retain SMEM after divergence analysis. The original OpenRR
selftest, including lane-varying indexing, is preserved and passes on ACO.

The historical version 3 policy preserved explicit FMA as strong NIR ffma and marked all supported
floating ALU controls exact before optimization, including precision conversions. Generic fast math otherwise removes
f32-to-f16-to-f32 rounding and splits FMA, breaking half_rz and reciprocal
correction. This eliminated the first-frame exposure mismatch in the diagnostic.

## ABI and descriptors

The layout range remains 256 bytes; effective D3D12 push data is limited to
244 bytes. Root dwords 0..60 map linearly to PAL entries 4..64. The continuous
push pointer is spill base +16; descriptor root dword 61 maps to entry 65.
NRPAL002 is the reused 96-byte header plus 8-byte inline pairs and machine code.
SPIR-V and code hashes use the existing PAL FNV variant, including its seed.

RADV combined image/sampler records use stride 64, image offset 0 and sampler
offset 48. Windows uses stride 48 and sampler offset 32. D3D12 therefore keeps
an ACO table for each recording slot and copies the sampler to the RADV offset.
The views layout matches: 16 sampled images at offset 0, stride 32; two storage
images at offset 512, stride 32. ACO ABI probes sample distinct array elements
0, 3 and 7, check all 61 push dwords and run the original selftest.

The adapter uses a clear_range wave32 PAL container for every imported entry.
Some LLPC network containers have relocations and auxiliary symbols, so they
cannot be used by this adapter. Each ACO record supplies its own workgroup
dimensions and resources; the small container's resources are replaced.

## Selection and validation

D3D12 requires ACO for every kernel and reports missing/invalid records as
failures. No per-shader LLPC fallback or performance enable list is used.
C API compiler values 0, 2 and 3 all require ACO on D3D12. Value 1 retains
LLPC only as an explicit diagnostic reference during this migration.
ONR_ACO_DIR overrides the record directory. Logs identify the actual compiler,
hash and record of each loaded kernel. The Windows driver compiles the PAL
container even when the imported shader body is ACO.

Use openrr_bench.exe --baseline-runtime <module> --baseline-compiler llpc
--candidate-compiler aco-strict for sequential contexts in one process.
The existing Fast quality thresholds and Legacy exact parity gate are retained.
Precision and performance regressions are debugging targets. They do not select
LLPC for production. The reference path can be removed after migration validation.

Full GPU validation remains necessary after source-level adaptation. Four historical enc_stage_96_128_128 variants
currently allocate 8 scratch bytes per thread. gfx1201 uses implicit hardware
scratch addressing, without a scratch SGPR in RADV's argument layout. PAL
receives scratch enable and per-thread size through compute metadata;
mismatched enable/size pairs are rejected.

The current patch also contains experimental cooperative-matrix global load
vectorization (up to eight contiguous column-major elements). It remains in the
review snapshot, with further operator tuning paused for review. The enc0_attr
shader packs existing FP16 bit patterns directly to avoid an observed LLPC
round-trip conversion corruption of the exposure attribute.
See docs/test-evidence/mesa-aco-review-2026-11-09.md for validation boundaries
and outstanding review items.
