#!/usr/bin/env python3
"""Apply the version-specific capture additions to a private Mesa checkout."""
import difflib
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1])
OUT = pathlib.Path(__file__).with_name("capture.patch")

EXPORT = r'''
/* OpenRR capture v4: source-level export, no serialized structure offsets. */
static void
onr_capture(const struct radv_shader_binary *binary, const struct radv_shader_args *args)
{
   const char *prefix = getenv("ONR_ACO_CAPTURE");
   if (!prefix || binary->info.stage != MESA_SHADER_COMPUTE || binary->type != RADV_BINARY_TYPE_LEGACY)
      return;
   char path[4096];
   struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)binary;
   struct radv_shader_binary_layout layout = radv_shader_binary_get_layout(bin);
   snprintf(path, sizeof(path), "%s.code", prefix);
   FILE *f = fopen(path, "wb");
   if (!f) abort();
   if (fwrite(layout.code, 1, bin->code_size, f) != bin->code_size || fclose(f)) abort();
   snprintf(path, sizeof(path), "%s.json", prefix);
   f = fopen(path, "w");
   if (!f) abort();
   const struct radv_shader_info *i = &binary->info;
   const struct ac_shader_config *c = &binary->config;
   const struct radv_userdata_locations *u = &args->user_sgprs_locs;
   fprintf(f, "{\"rsrc1\":%u,\"rsrc2\":%u,\"rsrc3\":%u,\"scratch_bytes\":%u,"
           "\"sgprs\":%u,\"vgprs\":%u,\"exec_bytes\":%u,\"workgroup_size\":%u,"
           "\"shared_bytes\":%u,\"push_bytes\":%u,\"wave_size\":%u,\"user_sgprs\":%u,"
           "\"set0\":%d,\"push\":%d,\"grid\":%d,\"inline\":[",
           c->rsrc1, c->rsrc2, c->rsrc3, c->scratch_bytes_per_wave / i->wave_size,
           c->num_sgprs, c->num_vgprs, bin->exec_size, i->workgroup_size,
           i->nir_shared_size, i->push_constant_size, i->wave_size, args->num_user_sgprs,
           u->descriptor_sets[0].sgpr_idx, u->shader_data[AC_UD_PUSH_CONSTANTS].sgpr_idx,
           u->shader_data[AC_UD_CS_GRID_SIZE].sgpr_idx);
   unsigned n = 0;
   u_foreach_bit64(dw, args->ac.inline_push_const_mask) {
      fprintf(f, "%s[%u,%u]", n ? "," : "",
              u->shader_data[AC_UD_INLINE_PUSH_CONSTANTS].sgpr_idx + n, (unsigned)dw);
      ++n;
   }
   fprintf(f, "]}\n");
   if (fclose(f)) abort();
}

'''

changes = []
p = ROOT / "src/amd/vulkan/radv_shader.c"
old = p.read_text()
assert "onr_capture" not in old, "capture patch already applied"
anchor_opts = "   ac_nir_set_options(compiler_info->ac, compiler_info->key.use_llvm, options);\n"
assert old.count(anchor_opts) == 1
new = old.replace(anchor_opts, anchor_opts + '''   const char *onr_prec_opt = getenv("ONR_ACO_PRECISION");
   if (onr_prec_opt && !strcmp(onr_prec_opt, "native") &&
       (options->float_mul_add16 & nir_float_muladd_support_has_ffma)) {
      options->float_mul_add16 |= nir_float_muladd_support_fuse;
   }
''')
anchor = "struct radv_shader_binary *\nradv_shader_nir_to_asm"
assert new.count(anchor) == 1
new = new.replace(anchor, EXPORT + anchor)
anchor = '      nir_validate_shader(nir, "after spirv_to_nir");'
assert new.count(anchor) == 1
new = new.replace(anchor, '''      /* OpenRR relies on explicit FMA and the rounding of f32->f16->f32.
       * Generic fast-math permits splitting weak FMA and removing conversions,
       * which breaks reciprocal correction and half_rz's round-trip test.
       */
      const char *onr_precision = getenv("ONR_ACO_PRECISION");
      const bool onr_global_exact = onr_precision && !strcmp(onr_precision, "global");
      const bool onr_native = onr_precision && !strcmp(onr_precision, "native");
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_alu) continue;
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (onr_global_exact)
                  alu->fp_math_ctrl |= nir_op_valid_fp_math_ctrl(alu->op, nir_fp_exact);
               if (alu->op == nir_op_ffma_weak && (!onr_native || alu->def.bit_size == 16)) {
                  alu->op = nir_op_ffma;
                  if (!onr_native)
                     alu->fp_math_ctrl |= nir_fp_exact;
               } else if (!onr_native && (alu->op == nir_op_f2f16 || alu->op == nir_op_f2f32 || alu->op == nir_op_f2f64)) {
                  alu->fp_math_ctrl |= nir_fp_exact;
               }
            }
         }
      }
''' + anchor)
anchor = "   return binary;\n}\n\nvoid\nradv_shader_dump_asm"
assert new.count(anchor) == 1
new = new.replace(anchor, "   onr_capture(binary, args);\n" + anchor)
changes.append((p, old, new))

p = ROOT / "src/amd/vulkan/radv_descriptor_set.c"
old = p.read_text()
anchor = "   *pSetLayout = radv_descriptor_set_layout_to_handle(set_layout);"
assert old.count(anchor) == 1
new = old.replace(anchor, r'''   const char *onr_prefix = getenv("ONR_ACO_CAPTURE");
   if (onr_prefix) {
      char path[4096];
      snprintf(path, sizeof(path), "%s.layouts", onr_prefix);
      FILE *f = fopen(path, "a");
      if (!f) abort();
      fprintf(f, "set bytes=%u bindings=%u\n", set_layout->size, set_layout->binding_count);
      for (unsigned b = 0; b < set_layout->binding_count; ++b) {
         const struct radv_descriptor_set_binding_layout *l = &set_layout->binding[b];
         fprintf(f, "binding=%u type=%u count=%u offset=%u stride=%u image=0 sampler=%u\n",
                 b, l->type, l->array_size, l->offset, l->size,
                 l->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? radv_get_combined_image_sampler_offset(pdev) : 0);
      }
      if (fclose(f)) abort();
   }
''' + anchor)
changes.append((p, old, new))

# Mesa 26.3 implements divergent push loads upstream.
p = ROOT / "src/amd/vulkan/nir/radv_nir_lower_cooperative_matrix.c"
old = p.read_text()
anchor = '''      if (is_load) {
         vars[i * mul] = nir_load_deref(b, iter_deref);
      } else {'''
assert old.count(anchor) == 1
new = old.replace(anchor, '''      /* OpenRR: same-lane run vectorization for cooperative-matrix fragments. In a
       * column-major fragment with mul == 1 the count elements a lane owns are strictly
       * contiguous in memory, global or shared alike; Mesa touches them one element at a
       * time. ONR_ACO_VECTOR_LOADS covers global loads; ONR_ACO_SHARED_CMAT extends the
       * same transformation to shared-memory loads ("load") and stores ("loadstore").
       * The cast keeps Mesa's own alignment annotations verbatim - never claims more
       * alignment than the per-element path, never crosses lanes, never changes layout.
       */
      const char *onr_shared_cmat = getenv("ONR_ACO_SHARED_CMAT");
      unsigned count = params->gfx_level >= GFX11_7 &&
                       layout == GLSL_MATRIX_LAYOUT_COLUMN_MAJOR && mul == 1 &&
                       ((getenv("ONR_ACO_VECTOR_LOADS") && is_load &&
                         nir_deref_mode_is(deref, nir_var_mem_global)) ||
                        (onr_shared_cmat &&
                         (is_load || strstr(onr_shared_cmat, "loadstore")) &&
                         nir_deref_mode_is(deref, nir_var_mem_shared))) ?
                       MIN2(8, length - i) : 1;
      if (is_load) {
         if (count > 1) {
            const struct glsl_type *vec_type = glsl_vector_type(desc.element_type, count);
            nir_deref_instr *vec = nir_build_deref_cast_with_alignment(
               b, &iter_deref->def, iter_deref->modes, vec_type,
               radv_nir_cmat_bits(desc) / 8, align_mul, row_iter * radv_nir_cmat_bits(desc) / 8 % align_mul);
            nir_def *value = nir_load_deref(b, vec);
            for (unsigned j = 0; j < count; ++j) vars[i + j] = nir_channel(b, value, j);
            i += count - 1;
         } else {
            vars[i * mul] = nir_load_deref(b, iter_deref);
         }
      } else if (count > 1 && onr_shared_cmat &&
                 strstr(onr_shared_cmat, "loadstore") &&
                 nir_deref_mode_is(deref, nir_var_mem_shared)) {
         /* Store path: pack the run and store it once with the same real alignment. */
         const struct glsl_type *vec_type = glsl_vector_type(desc.element_type, count);
         nir_deref_instr *vec = nir_build_deref_cast_with_alignment(
            b, &iter_deref->def, iter_deref->modes, vec_type,
            radv_nir_cmat_bits(desc) / 8, align_mul, row_iter * radv_nir_cmat_bits(desc) / 8 % align_mul);
         nir_def *packed = nir_vec(b, &vars[i * mul], count);
         nir_store_deref(b, vec, packed, BITFIELD_MASK(count));
         i += count - 1;
      } else {''')
changes.append((p, old, new))

# Source-level address32 adaptation, ported from OpenNR's record producer.
# Counters belong to each program, avoiding shared mutable capture state.
p = ROOT / "src/amd/compiler/aco_assembler.cpp"
old = p.read_text()
new = '#include <cstdlib>\n#include <cstdio>\n' + old
anchor = 'struct asm_context {'
assert new.count(anchor) == 1
new = new.replace(anchor, anchor + '''
   uint32_t onr_sentinel = getenv("ONR_ACO_ADDR32_HI") ?
      strtoul(getenv("ONR_ACO_ADDR32_HI"), nullptr, 0) : 0;
   unsigned onr_m0_subst = 0, onr_m0_writes = 0, onr_pc_relative = 0;
''')
anchor = 'emit_instruction(asm_context& ctx, std::vector<uint32_t>& out, Instruction* instr)\n{'
assert new.count(anchor) == 1
new = new.replace(anchor, anchor + '''
   if (ctx.onr_sentinel) {
      for (Operand& op : instr->operands) {
         if (op.isConstant() && op.bytes() == 4 && op.constantValue() == ctx.onr_sentinel) {
            op = Operand(m0, s1);
            ctx.onr_m0_subst++;
         }
      }
      for (const Definition& d : instr->definitions)
         if (d.isFixed() && d.physReg() == m0) ctx.onr_m0_writes++;
      if (instr->opcode == aco_opcode::p_constaddr_getpc ||
          instr->opcode == aco_opcode::s_getpc_b64 ||
          instr->opcode == aco_opcode::s_setpc_b64 ||
          instr->opcode == aco_opcode::s_swappc_b64) ctx.onr_pc_relative++;
   }
''')
anchor = '   fix_branches(ctx, code);'
assert new.count(anchor) == 1
new = new.replace(anchor, anchor + '''
   if (ctx.onr_sentinel)
      fprintf(stderr, "ONR_M0 subst=%u writes=%u constdata=%u pc_relative=%u\\n",
              ctx.onr_m0_subst, ctx.onr_m0_writes,
              (unsigned)program->constant_data.size(), ctx.onr_pc_relative);
''')
changes.append((p, old, new))

p = ROOT / "src/amd/vulkan/radv_device.c"
old = p.read_text()
anchor = '.address32_hi = pdev->info.address32_hi,'
assert old.count(anchor) == 1
new = old.replace(anchor, '''.address32_hi = getenv("ONR_ACO_ADDR32_HI") ?
               strtoul(getenv("ONR_ACO_ADDR32_HI"), NULL, 0) : pdev->info.address32_hi,''')
changes.append((p, old, new))

# ONR_ACO_CU_MODE / ONR_ACO_WGP_MODE: per-shader CU/WGP mode override at compile time
# so ACO scheduling, waitcnt, and PGM rsrc1 bit 29 all agree. Composed onto the existing
# radv_shader.c change entry (never add a duplicate radv_shader.c tuple).
a1 = """   if (gfx_level < GFX10)
      return false;
"""
for _i, (_p, _old, _new) in enumerate(changes):
    if _p == ROOT / "src/amd/vulkan/radv_shader.c":
        assert _new.count(a1) == 1, "wgp gate anchor not found"
        composed = _new.replace(a1, a1 + """
   /* OpenRR capture: per-shader CU/WGP mode override */
   if (stage == MESA_SHADER_COMPUTE) {
      if (getenv("ONR_ACO_CU_MODE") && getenv("ONR_ACO_CU_MODE")[0] != '0')
         return false;
      if (getenv("ONR_ACO_WGP_MODE") && getenv("ONR_ACO_WGP_MODE")[0] != '0')
         return true;
   }
""", 1)
        changes[_i] = (_p, _old, composed)
        break
else:
    raise SystemExit("radv_shader.c change entry missing")

# ONR_ACO_SAMPLE_WAIT_ZERO: coalesce GFX12 sampler waits to s_wait_samplecnt 0x0 instead
# of decrementing partial counts 0x3->0x2->0x1->0x0 across consecutive sample reads.
p = ROOT / "src/amd/compiler/aco_insert_waitcnt.cpp"
old = p.read_text()
anc_inc = "#include <map>\n"
assert old.count(anc_inc) == 1, "aco_insert_waitcnt include anchor not found"
new = old.replace(anc_inc, "#include <cstdlib>\n" + anc_inc, 1)
anc_cnt = "      for (unsigned i = 0; i < wait_type_num; i++)\n         max_cnt[i] = max_cnt[i] ? max_cnt[i] - 1 : 0;\n"
assert new.count(anc_cnt) == 1, "aco_insert_waitcnt max_cnt anchor not found"
new = new.replace(anc_cnt, anc_cnt + """      if (gfx_level >= GFX12 && std::getenv("ONR_ACO_SAMPLE_WAIT_ZERO") &&
          std::getenv("ONR_ACO_SAMPLE_WAIT_ZERO")[0] != '0')
         max_cnt[wait_type_sample] = 0;
""", 1)
changes.append((p, old, new))

# RX 9060 / 9060 XT (gfx1200 / CHIP_GFX1200) noop-drm-shim entry + e_flags propagation
p = ROOT / "src/amd/common/amdgpu_devices.c"
old = p.read_text()
anc_g12 = '   {\n      .name = "gfx1201",\n      .radeon_family = CHIP_GFX1201,\n'
assert old.count(anc_g12) == 1, "gfx1201 anchor in amdgpu_devices.c not found"
gfx1200_entry = '''   {
      .name = "gfx1200",
      .radeon_family = CHIP_GFX1200,
      .hw_ip_gfx = {
         .hw_ip_version_major = 12,
         .hw_ip_version_minor = 0,
         .capabilities_flags = UINT64_C(0),
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0x1,
         .ip_discovery_version = 0xc0001,
      },
      .hw_ip_compute = {
         .hw_ip_version_major = 12,
         .hw_ip_version_minor = 0,
         .capabilities_flags = UINT64_C(0),
         .ib_start_alignment = 32,
         .ib_size_alignment = 32,
         .available_rings = 0xf,
         .ip_discovery_version = 0xc0001,
      },
      .fw_gfx_me = {
         .ver = 2590,
         .feature = 29,
      },
      .fw_gfx_pfp = {
         .ver = 2630,
         .feature = 29,
      },
      .fw_gfx_mec = {
         .ver = 2800,
         .feature = 29,
      },
      .mmr_regs = {
         0x263e, 0xffffffff, 0x08200545,
      },
      .mmr_reg_count = 1,
      .dev = {
         .device_id = 0x7590,
         .chip_rev = 0x01,
         .external_rev = 0x41,
         .pci_rev = 0xc0,
         .family = AMDGPU_FAMILY_GC_12_0_0,
         .num_shader_engines = 2,
         .num_shader_arrays_per_engine = 2,
         .gpu_counter_freq = 100000,
         .max_engine_clock = UINT64_C(2460000),
         .max_memory_clock = UINT64_C(1258000),
         .cu_active_number = 32,
         .cu_ao_mask = 0x0,
         .cu_bitmap = {
            { 0xff, 0xff, 0x0, 0x0, },
            { 0xff, 0xff, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .enabled_rb_pipes_mask = 0xff,
         .num_rb_pipes = 8,
         .num_hw_gfx_contexts = 8,
         .pcie_gen = 4,
         .ids_flags = UINT64_C(0x0),
         .virtual_address_offset = UINT64_C(0x10000),
         .virtual_address_max = UINT64_C(0x800000000000),
         .virtual_address_alignment = 4096,
         .pte_fragment_size = 2097152,
         .gart_page_size = 4096,
         .ce_ram_size = 0,
         .vram_type = 9,
         .vram_bit_width = 128,
         .vce_harvest_config = 0,
         .gc_double_offchip_lds_buf = 64,
         .prim_buf_gpu_addr = UINT64_C(0),
         .pos_buf_gpu_addr = UINT64_C(0),
         .cntl_sb_buf_gpu_addr = UINT64_C(0),
         .param_buf_gpu_addr = UINT64_C(0),
         .prim_buf_size = 0,
         .pos_buf_size = 0,
         .cntl_sb_buf_size = 0,
         .param_buf_size = 0,
         .wave_front_size = 32,
         .num_shader_visible_vgprs = 1536,
         .num_cu_per_sh = 8,
         .num_tcc_blocks = 16,
         .gs_vgt_table_depth = 32,
         .gs_prim_buffer_depth = 1792,
         .max_gs_waves_per_vgt = 32,
         .pcie_num_lanes = 8,
         .cu_ao_bitmap = {
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
            { 0x0, 0x0, 0x0, 0x0, },
         },
         .high_va_offset = UINT64_C(0xffff800000000000),
         .high_va_max = UINT64_C(0xffffffffffbfe000),
         .pa_sc_tile_steering_override = 0,
         .tcc_disabled_mask = UINT64_C(0),
         .min_engine_clock = UINT64_C(500000),
         .min_memory_clock = UINT64_C(96000),
         .tcp_cache_size = 32,
         .num_sqc_per_wgp = 1,
         .sqc_data_cache_size = 16,
         .sqc_inst_cache_size = 32,
         .gl1c_cache_size = 256,
         .gl2c_cache_size = 4096,
         .mall_size = UINT64_C(33554432),
         .enabled_rb_pipes_mask_hi = 0,
      },
      .mem = {
         .vram = {
            .total_heap_size = UINT64_C(17095983104),
            .usable_heap_size = UINT64_C(16543346688),
            .heap_usage = UINT64_C(555782144),
            .max_allocation = UINT64_C(12407510016),
         },
         .cpu_accessible_vram = {
            .total_heap_size = UINT64_C(268435456),
            .usable_heap_size = UINT64_C(265252864),
            .heap_usage = UINT64_C(4526080),
            .max_allocation = UINT64_C(198939648),
         },
         .gtt = {
            .total_heap_size = UINT64_C(16785289216),
            .usable_heap_size = UINT64_C(16769716224),
            .heap_usage = UINT64_C(15622144),
            .max_allocation = UINT64_C(12577287168),
         },
      },
   },
'''
new = old.replace(anc_g12, gfx1200_entry + anc_g12, 1)
changes.append((p, old, new))

p = ROOT / "src/amd/common/ac_gpu_info.h"
old = p.read_text()
anc_h = "   uint32_t reserved : 2;\n};"
assert old.count(anc_h) == 1, "reserved : 2 anchor in ac_gpu_info.h not found"
new = old.replace(anc_h, "   uint32_t is_gfx1200 : 1;\n   uint32_t reserved : 1;\n};", 1)
changes.append((p, old, new))

p = ROOT / "src/amd/common/ac_gpu_info.c"
old = p.read_text()
anc_c = "   out->gfx_level = info->gfx_level;\n"
assert old.count(anc_c) == 1, "ac_fill_compiler_info anchor in ac_gpu_info.c not found"
new = old.replace(anc_c, "   out->gfx_level = info->gfx_level;\n   out->is_gfx1200 = info->family == CHIP_GFX1200;\n", 1)
changes.append((p, old, new))

p = ROOT / "src/amd/compiler/aco_elf.cpp"
old = p.read_text()
anc_elf = "   case GFX12: return EF_AMDGPU_MACH_AMDGCN_GFX1201;"
assert old.count(anc_elf) == 1, "GFX12 anchor in aco_elf.cpp not found"
new = old.replace(anc_elf, "   case GFX12: return (compiler_info && compiler_info->is_gfx1200) ? 0x48 /* EF_AMDGPU_MACH_AMDGCN_GFX1200 */ : EF_AMDGPU_MACH_AMDGCN_GFX1201;", 1)
changes.append((p, old, new))

patch = []
for p, old, new in changes:
    rel = p.relative_to(ROOT).as_posix()
    patch.extend(difflib.unified_diff(old.splitlines(True), new.splitlines(True), "a/"+rel, "b/"+rel))
    p.write_text(new)
OUT.write_text("".join(patch))
