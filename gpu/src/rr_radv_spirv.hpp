// In-memory SPIR-V precision hardener for system Mesa RADV (Vulkan) without a patched Mesa binary.
// Mirrors OpenRR's offline ACO precision split (build_asi.sh: global exact vs 37 native-only *_lt/*_lx kernels)
// by attaching SpvDecorationNoContraction to target arithmetic and FP conversion result IDs in SPIR-V.
// In Mesa spirv_to_nir (vtn_alu.c / vtn_glsl450.c), SpvDecorationNoContraction sets nir_fp_exact, preserving
// f32<->f16 conversion rounding (half_rz) and preventing ffma_weak contraction splits on stock RADV.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace onr {

inline bool is_radv_native_precision_stem(const std::string& stem) {
    static const std::unordered_set<std::string> kNativeOnly = {
        "post_f_lt", "d3d_enc0_attr_f_lt", "d3d_enc0_attr_f_w_lt", "enc0_attr_f_lt", "enc0_attr_f_w_lt",
        "post_dlt_f_lt", "dec_head_32_64_f_lt", "dec_head_32_64_f_lx", "enc0_stage_32_64_f_lt",
        "enc0_stage_32_64_f_lx", "enc0_stage_32_64_f_w_lt", "enc0_stage_32_64_f_w_lx", "dec_up_32_64_64_f_lt",
        "dec_up_32_64_64_f_lx", "enc_stage_64_64_96_f_lt", "dec_up_64_64_96_f_lt", "dec_dfn_64_64_f_lt",
        "enc_stage_96_128_128_f_lt", "enc_stage_96_128_128_fl_lt", "dec_up_96_128_128_f_lt",
        "dec_up_96_128_128_fl_lt", "dec_dfn_96_128_f_lt", "dec_dfn_96_128_fl_lt", "enc_stage_128_128_160_f_lt",
        "enc_stage_128_128_160_fl_lt", "dec_up_128_128_160_fl_lt", "dec_up_128_128_160_f_lt",
        "dec_dfn_128_128_f_lt", "dec_dfn_128_128_fl_lt", "dec_dfn_160_256_fl_lt", "dec_dfn_160_256_f_lt",
        "enc_stage_160_256_0_fl_lt", "enc_stage_160_256_0_f_lt", "enc_stage_160_256_160_fl_lt",
        "enc_stage_160_256_160_f_lt", "dec_up_160_256_160_f_lt", "dec_up_160_256_160_fl_lt",
    };
    return kNativeOnly.count(stem) != 0;
}

inline std::vector<uint32_t> harden_spirv_for_radv(const uint8_t* bytes, size_t byte_len, const std::string& stem,
                                                   uint32_t* out_added_decorations = nullptr) {
    if (out_added_decorations) *out_added_decorations = 0;
    if (!bytes || byte_len < 20 || (byte_len % 4) != 0) return {};

    std::vector<uint32_t> words(byte_len / 4);
    std::memcpy(words.data(), bytes, byte_len);
    constexpr uint32_t kSpvMagic = 0x07230203u;
    if (words[0] != kSpvMagic) return words;

    const bool native_mode = is_radv_native_precision_stem(stem);

    // SPIR-V constants (Khronos unified specification).
    constexpr uint32_t kOpExtInstImport = 11u;
    constexpr uint32_t kOpExtInst = 12u;
    constexpr uint32_t kOpTypeFloat = 22u;
    constexpr uint32_t kOpTypeVector = 23u;
    constexpr uint32_t kOpTypeMatrix = 24u;
    constexpr uint32_t kOpFunction = 54u;
    constexpr uint32_t kOpFunctionEnd = 56u;
    constexpr uint32_t kOpDecorate = 71u;
    constexpr uint32_t kOpMemberDecorate = 72u;
    constexpr uint32_t kOpDecorationGroup = 73u;
    constexpr uint32_t kOpGroupDecorate = 74u;
    constexpr uint32_t kOpGroupMemberDecorate = 75u;
    constexpr uint32_t kOpDecorateId = 332u;
    constexpr uint32_t kOpDecorateString = 5632u;
    constexpr uint32_t kOpMemberDecorateString = 5633u;
    constexpr uint32_t kDecNoContraction = 42u;
    constexpr uint32_t kGlslFma = 50u;

    auto is_header_or_debug_or_annot = [](uint32_t op) {
        switch (op) {
            case 2: case 3: case 4: case 5: case 6: case 7: case 8:
            case 10: case 11: case 14: case 15: case 16: case 17:
            case 71: case 72: case 73: case 74: case 75:
            case 317: case 330: case 331: case 332: case 5632: case 5633:
                return true;
            default:
                return false;
        }
    };
    auto is_annotation = [](uint32_t op) {
        return op == kOpDecorate || op == kOpMemberDecorate || op == kOpDecorationGroup ||
               op == kOpGroupDecorate || op == kOpGroupMemberDecorate ||
               op == kOpDecorateId || op == kOpDecorateString || op == kOpMemberDecorateString;
    };
    auto is_global_fp_op = [](uint32_t op) {
        switch (op) {
            case 115: // OpFConvert
            case 116: // OpQuantizeToF16
            case 127: // OpFNegate
            case 129: // OpFAdd
            case 131: // OpFSub
            case 133: // OpFMul
            case 136: // OpFDiv
            case 139: // OpFRem
            case 140: // OpFMod
            case 142: // OpVectorTimesScalar
            case 143: // OpMatrixTimesScalar
            case 144: // OpVectorTimesMatrix
            case 145: // OpMatrixTimesVector
            case 146: // OpMatrixTimesMatrix
            case 147: // OpOuterProduct
            case 148: // OpDot
                return true;
            default:
                return false;
        }
    };
    auto is_glsl_fp_op = [](uint32_t glsl_op) {
        // Every floating-point ALU / math instruction in GLSL.std.450 (excluding Pack/Unpack/Int/Bit ops).
        if (glsl_op >= 13u && glsl_op <= 34u) return true; // Sin..Atanh, Pow..InverseSqrt, Determinant, MatrixInverse
        switch (glsl_op) {
            case 1: case 2: case 3: case 4: case 6: case 8: case 9: case 10: case 11: case 12:
            case 35: case 37: case 40: case 43: case 46: case 48: case 49: case 50: case 51: case 53:
            case 66: case 67: case 68: case 69: case 70: case 71: case 72: case 79: case 80: case 81:
                return true;
            default:
                return false;
        }
    };

    uint32_t glsl450_id = 0;
    std::unordered_set<uint32_t> f16_types, fp_types, existing_nc;
    size_t insert_word_idx = 0;
    bool found_insert_point = false;
    bool in_function = false;
    std::vector<uint32_t> add_ids;
    std::unordered_set<uint32_t> queued_ids;

    const size_t n = words.size();
    for (size_t i = 5; i < n;) {
        const uint32_t header = words[i];
        const uint32_t wc = header >> 16;
        const uint32_t op = header & 0xFFFFu;
        if (wc == 0 || i + wc > n) return words; // Malformed stream: leave untouched.

        if (!found_insert_point && !is_header_or_debug_or_annot(op)) {
            insert_word_idx = i;
            found_insert_point = true;
        }
        if (is_annotation(op) && !found_insert_point) {
            insert_word_idx = i + wc;
        }

        if (op == kOpExtInstImport && wc >= 3) {
            const char* name = reinterpret_cast<const char*>(&words[i + 2]);
            const size_t max_bytes = (wc - 2) * 4;
            if (max_bytes >= 13 && std::strncmp(name, "GLSL.std.450", max_bytes) == 0) {
                glsl450_id = words[i + 1];
            }
        } else if (op == kOpDecorate && wc >= 3 && words[i + 2] == kDecNoContraction) {
            existing_nc.insert(words[i + 1]);
        } else if (op == kOpTypeFloat && wc >= 3) {
            const uint32_t tid = words[i + 1];
            const uint32_t width = words[i + 2];
            if (width == 16 || width == 32 || width == 64) {
                fp_types.insert(tid);
                if (width == 16) f16_types.insert(tid);
            }
        } else if ((op == kOpTypeVector || op == kOpTypeMatrix) && wc >= 4) {
            const uint32_t tid = words[i + 1];
            const uint32_t elem = words[i + 2];
            if (fp_types.count(elem)) fp_types.insert(tid);
            if (f16_types.count(elem)) f16_types.insert(tid);
        } else if (op == kOpFunction) {
            in_function = true;
        } else if (op == kOpFunctionEnd) {
            in_function = false;
        } else if (in_function && wc >= 3) {
            const uint32_t rtype = words[i + 1];
            const uint32_t rid = words[i + 2];
            bool decorate = false;
            if (native_mode) {
                if (op == kOpExtInst && wc >= 5 && words[i + 3] == glsl450_id && words[i + 4] == kGlslFma &&
                    f16_types.count(rtype)) {
                    decorate = true;
                }
            } else {
                if (fp_types.count(rtype)) {
                    if (is_global_fp_op(op)) {
                        decorate = true;
                    } else if (op == kOpExtInst && wc >= 5 && words[i + 3] == glsl450_id &&
                               is_glsl_fp_op(words[i + 4])) {
                        decorate = true;
                    }
                }
            }
            if (decorate && !existing_nc.count(rid) && queued_ids.insert(rid).second) {
                add_ids.push_back(rid);
            }
        }

        i += wc;
    }

    if (add_ids.empty() || !found_insert_point) return words;

    std::vector<uint32_t> out;
    out.reserve(words.size() + add_ids.size() * 3);
    out.insert(out.end(), words.begin(), words.begin() + std::ptrdiff_t(insert_word_idx));
    for (uint32_t id : add_ids) {
        out.push_back((3u << 16) | kOpDecorate);
        out.push_back(id);
        out.push_back(kDecNoContraction);
    }
    out.insert(out.end(), words.begin() + std::ptrdiff_t(insert_word_idx), words.end());
    if (out_added_decorations) *out_added_decorations = uint32_t(add_ids.size());
    return out;
}

}  // namespace onr
