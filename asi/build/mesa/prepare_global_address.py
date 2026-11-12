#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent opt-in experiment atop capturev4; only bounded cmat lane offsets."""
from pathlib import Path
import sys,difflib
root=Path(sys.argv[1]);p=root/'src/amd/vulkan/nir/radv_nir_lower_cooperative_matrix.c'
old=p.read_text();assert 'ONR_ACO_CMAT_ADDRESS' not in old
anchor='''      if (align_mul) {
         unsigned align_offset = row_iter * radv_nir_cmat_bits(desc) / 8 % align_mul;'''
assert old.count(anchor)==1
new=old.replace(anchor,'''      /* OpenRR experiment: preserve the original 64-bit pointer, including
       * signed source indices. Only combine the generated nonnegative lane
       * offsets, after proving both the original product and byte sum fit u32.
       * No stores, transpose loads, shared memory, or dynamic strides.
       */
      if (getenv("ONR_ACO_CMAT_ADDRESS") && is_load &&
          getenv("ONR_ACO_VECTOR_LOADS") && params->gfx_level >= GFX11_7 &&
          params->wave_size == 32 && layout == GLSL_MATRIX_LAYOUT_COLUMN_MAJOR &&
          mul == 1 && length == 8 && ptr_stride == radv_nir_cmat_bits(desc) / 8 &&
          (ptr_stride == 1 || ptr_stride == 2) &&
          nir_deref_mode_is(deref, nir_var_mem_global) && nir_src_is_const(intr->src[2])) {
         uint64_t stride_value = nir_src_as_uint(intr->src[2]);
         unsigned elem_bytes = radv_nir_cmat_bits(desc) / 8;   /* 1 (fp8) or 2 (fp16) */
         /* inner_idx <= 15, base_row <= 8, row_iter <= 7, elem_bytes <= 2.
          * Worst byte sum 15*stride*elem + 15*elem <= 30*stride + 30 fits u32
          * under the bound below, which also proves the original 32-bit
          * inner_idx*stride cannot overflow.
          */
         if (stride_value <= (UINT32_MAX - 30u) / 30u) {
            nir_def *column = nir_imul_imm(b, inner_idx, stride_value * elem_bytes);
            nir_def *row = nir_imul_imm(b, nir_iadd_imm(b, base_row, row_iter), elem_bytes);
            nir_def *bytes = nir_iadd_nuw(b, column, row);
            nir_deref_instr *byte_ptr = nir_build_deref_cast(
               b, &deref->def, deref->modes, glsl_uint8_t_type(), 1);
            byte_ptr = nir_build_deref_ptr_as_array(b, byte_ptr, nir_u2u64(b, bytes));
            iter_deref = nir_build_deref_cast(b, &byte_ptr->def, deref->modes,
                                            glsl_scalar_type(desc.element_type), elem_bytes);
         }
      }

''' + anchor)
p.write_text(new)
out=Path('asi/build/mesa/global-address.patch')
out.write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/src/amd/vulkan/nir/'+p.name,tofile='b/src/amd/vulkan/nir/'+p.name)))
print(out)
