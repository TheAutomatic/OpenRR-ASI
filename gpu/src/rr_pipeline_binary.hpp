// Native Vulkan ACO compute pipeline loader via VK_KHR_pipeline_binary.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "opennr_rr/gpu_arch.hpp"
#include "opennr_rr/vk.hpp"
#include "rr_pal_binary.hpp"

namespace onr {

struct AcoFallbackError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct FatalVkError : std::runtime_error {
    VkResult result = VK_SUCCESS;
    FatalVkError(const std::string& msg, VkResult r) : std::runtime_error(msg), result(r) {}
};

namespace detail_pbin {

inline std::string hex64(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

inline std::string hex32(uint32_t v) {
    char b[9];
    std::snprintf(b, sizeof b, "%08x", static_cast<unsigned>(v));
    return b;
}

inline bool is_fatal_vk(VkResult r) {
    return r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_DEVICE_LOST;
}

inline void check_vk(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    const std::string msg = std::string(what) + " failed (" + std::to_string(int(r)) + ")";
    if (is_fatal_vk(r)) throw FatalVkError(msg, r);
    throw AcoFallbackError(msg);
}

inline bool atomic_replace_file(const std::filesystem::path& tmp, const std::filesystem::path& dst) {
#ifdef _WIN32
    return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    return !ec;
#endif
}

struct PbinProcs {
    PFN_vkCreatePipelineBinariesKHR create = nullptr;
    PFN_vkDestroyPipelineBinaryKHR destroy = nullptr;
    PFN_vkGetPipelineBinaryDataKHR data = nullptr;
    PFN_vkGetPipelineKeyKHR key = nullptr;
    PFN_vkReleaseCapturedPipelineDataKHR release = nullptr;

    bool load(VkDevice dev) {
        create = reinterpret_cast<PFN_vkCreatePipelineBinariesKHR>(vkGetDeviceProcAddr(dev, "vkCreatePipelineBinariesKHR"));
        destroy = reinterpret_cast<PFN_vkDestroyPipelineBinaryKHR>(vkGetDeviceProcAddr(dev, "vkDestroyPipelineBinaryKHR"));
        data = reinterpret_cast<PFN_vkGetPipelineBinaryDataKHR>(vkGetDeviceProcAddr(dev, "vkGetPipelineBinaryDataKHR"));
        key = reinterpret_cast<PFN_vkGetPipelineKeyKHR>(vkGetDeviceProcAddr(dev, "vkGetPipelineKeyKHR"));
        release = reinterpret_cast<PFN_vkReleaseCapturedPipelineDataKHR>(vkGetDeviceProcAddr(dev, "vkReleaseCapturedPipelineDataKHR"));
        return create && destroy && data && key && release;
    }
};

struct BinaryList {
    VkDevice dev = VK_NULL_HANDLE;
    const PbinProcs& fn;
    std::vector<VkPipelineBinaryKHR> handles;
    BinaryList(VkDevice d, const PbinProcs& f) : dev(d), fn(f) {}
    ~BinaryList() {
        for (VkPipelineBinaryKHR h : handles)
            if (h && fn.destroy) fn.destroy(dev, h, nullptr);
    }
    BinaryList(const BinaryList&) = delete;
    BinaryList& operator=(const BinaryList&) = delete;
};

struct CapturedPipe {
    VkDevice dev = VK_NULL_HANDLE;
    const PbinProcs& fn;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool captured = false;
    CapturedPipe(VkDevice d, const PbinProcs& f) : dev(d), fn(f) {}
    ~CapturedPipe() {
        if (pipe && captured && fn.release) {
            VkReleaseCapturedPipelineDataInfoKHR ri{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR};
            ri.pipeline = pipe;
            fn.release(dev, &ri, nullptr);
        }
        if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
        if (mod) vkDestroyShaderModule(dev, mod, nullptr);
    }
    CapturedPipe(const CapturedPipe&) = delete;
    CapturedPipe& operator=(const CapturedPipe&) = delete;
};

}  // namespace detail_pbin

class PipelineBinaryLoader {
public:
    struct Result {
        VkPipeline pipe = VK_NULL_HANDLE;
        std::string nrp_path;
        std::string manifest_path;
        uint64_t code_hash = 0;
        uint32_t rsrc1 = 0;
        uint32_t scratch_bytes = 0;
        bool cache_hit = false;
    };

    PipelineBinaryLoader(VkDevice dev, uint32_t elf_mach, bool device_pbin_enabled,
                         std::string cache_dir, std::string shader_dir)
        : dev_(dev),
          cache_base_(std::move(cache_dir)),
          shader_dir_(std::move(shader_dir)) {
        init(elf_mach, device_pbin_enabled);
    }

    bool ready() const { return ready_; }
    const std::string& reason() const { return reason_; }
    const std::string& target() const { return target_; }
    uint32_t cache_hits() const { return hits_.load(std::memory_order_relaxed); }
    uint32_t cache_misses() const { return misses_.load(std::memory_order_relaxed); }

    Result load(const pal::Bytes& spv, const std::string& spv_path, uint32_t subgroup,
                VkPipelineLayout layout, VkPipelineCreateFlags pipeline_flags, bool views) {
        if (!ready_) throw AcoFallbackError(reason_.empty() ? "Vulkan ACO loader not initialized" : reason_);
        pal::require(!subgroup || subgroup == 32, "Vulkan ACO requires subgroup=32 or 0");

        const uint64_t spv_hash_val = pal::hash(spv.data(), spv.size());
        const std::string spv_key = detail_pbin::hex64(spv_hash_val);

        const auto& records = index_.at("records");
        const auto& entry = records.at(spv_key);
        pal::require(entry.at("spv_hash").s == spv_key && entry.at("record").s == spv_key + ".nrp" &&
                     entry.at("layout").s == (views ? "views" : "sampled"),
                     "ACO record identity mismatch");

        const std::filesystem::path nrp_fs = root_ / (spv_key + ".nrp");
        const std::string nrp_path = path_to_u8(nrp_fs);
        const pal::Bytes nrp_bytes = pal::load(nrp_path);
        const pal::Record record = pal::Record::parse(nrp_bytes);
        pal::require(record.spv_hash == spv_hash_val &&
                     detail_pbin::hex64(record.code_hash) == entry.at("code_hash").s &&
                     record.mach == expected_mach_ && record.push_bytes <= 256,
                     "ACO record does not match SPIR-V/target/ABI");
        pal::require(record.scratch_en == (record.scratch_bytes != 0) && record.scratch_bytes % 4 == 0,
                     "ACO scratch configuration mismatch");

        const auto& dims = entry.at("workgroup");
        pal::require(dims.kind == pal::Value::Array && dims.items.size() == 3, "ACO workgroup dimensions");
        std::array<uint32_t, 3> wg{};
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t n = dims.items[i].number();
            pal::require(n > 0 && n <= 1024, "ACO workgroup dimension range");
            wg[i] = uint32_t(n);
        }

        const pal::Bytes& tmpl_spv = views ? views_tmpl_spv_ : sampled_tmpl_spv_;
        const uint64_t cache_id = compute_cache_id(spv_hash_val, record.code_hash,
                                                   pal::hash(nrp_bytes.data(), nrp_bytes.size()),
                                                   pal::hash(tmpl_spv.data(), tmpl_spv.size()),
                                                   views, wg, pipeline_flags);
        std::filesystem::path cache_file;
        if (!cache_dir_.empty())
            cache_file = cache_dir_ / (detail_pbin::hex64(cache_id) + ".nrb");

        if (!cache_file.empty()) {
            VkPipelineBinaryKeyKHR cached_key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            pal::Bytes cached_blob;
            if (load_nrb(cache_file, cached_key, cached_blob)) {
                try {
                    VkPipeline pipe = import_pipeline(layout, pipeline_flags, cached_key, cached_blob);
                    hits_.fetch_add(1, std::memory_order_relaxed);
                    return Result{pipe, nrp_path, manifest_path_, record.code_hash, record.rsrc1, record.scratch_bytes, true};
                } catch (const FatalVkError&) {
                    throw;
                } catch (const std::exception&) {
                    std::error_code ec;
                    std::filesystem::remove(cache_file, ec);
                }
            }
        }

        const pal::Bytes& tmpl_elf = ensure_template(views, layout, pipeline_flags);
        const pal::Bytes spliced = pal::splice_vulkan(tmpl_elf, record, 256, wg);

#ifdef OPENRR_TEST_HOOKS
        if (const char* dump = std::getenv("ONR_ACO_DUMP")) {
            if (*dump) {
                std::error_code ec;
                std::filesystem::create_directories(u8_to_path(dump), ec);
                std::ofstream ft(u8_to_path(dump) / (spv_key + ".vk_template.elf"), std::ios::binary);
                ft.write(reinterpret_cast<const char*>(tmpl_elf.data()), std::streamsize(tmpl_elf.size()));
                std::ofstream fs(u8_to_path(dump) / (spv_key + ".vk_spliced.elf"), std::ios::binary);
                fs.write(reinterpret_cast<const char*>(spliced.data()), std::streamsize(spliced.size()));
            }
        }
#endif

        VkPipelineBinaryKeyKHR key = derive_binary_key(spliced, record.spv_hash);
        VkPipeline pipe = import_pipeline(layout, pipeline_flags, key, spliced);
        if (!cache_file.empty()) save_nrb(cache_file, key, spliced);
        misses_.fetch_add(1, std::memory_order_relaxed);
        (void)spv_path;
        return Result{pipe, nrp_path, manifest_path_, record.code_hash, record.rsrc1, record.scratch_bytes, false};
    }

private:
    void init(uint32_t elf_mach, bool device_pbin_enabled) {
        try {
            if (!dev_) throw AcoFallbackError("null VkDevice");
            if (!device_pbin_enabled)
                throw AcoFallbackError("VK_KHR_pipeline_binary + maintenance5 not enabled on VkDevice");
            if (!fn_.load(dev_))
                throw AcoFallbackError("VK_KHR_pipeline_binary device entry points unavailable");

            const char* env_target = nullptr;
#ifdef OPENRR_TEST_HOOKS
            env_target = std::getenv("ONR_ACO_TARGET");
#endif
            const char* resolved = (env_target && *env_target) ? env_target : onr::arch::aco_target_for_mach(elf_mach);
            if (!resolved) throw AcoFallbackError("no offline ACO target for GPU");
            target_ = resolved;
            expected_mach_ = (target_ == "gfx1200") ? 0x48u : (target_ == "gfx1201") ? 0x4eu : 0u;
            if (!expected_mach_) throw AcoFallbackError("unsupported offline ACO target: " + target_);
            env_target_override_ = (env_target && *env_target);

            std::string sd = shader_dir_;
            while (!sd.empty() && (sd.back() == '/' || sd.back() == '\\')) sd.pop_back();
            root_ = u8_to_path(sd).parent_path() / "aco" / target_ / "openrr-d3d12-v1";
#ifdef OPENRR_TEST_HOOKS
            if (const char* dir = std::getenv("ONR_ACO_DIR"))
                if (*dir) root_ = u8_to_path(dir);
#endif

            manifest_path_ = path_to_u8(root_ / "manifest.json");
            const pal::Bytes msg = pal::load(path_to_u8(root_ / "manifest.msgpack"));
            index_ = pal::unpack(msg);
            auto req_num = [&](const char* field, uint64_t expect) {
                pal::require(index_.at(field).number() == expect, "ACO manifest configuration mismatch");
            };
            req_num("schema", 1);
            req_num("wave_size", 32);
            req_num("layout_push_bytes", 256);
            const uint64_t pv = index_.at("patch_version").number();
            pal::require(pv == 3 || pv == 4 || pv == 5, "unsupported ACO capture patch version");
            pal::require(index_.at("target").s == target_ && index_.at("layout_version").s == "openrr-d3d12-v1" &&
                         index_.at("mesa_commit").s == "0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4",
                         "ACO manifest provenance mismatch");
            const pal::Bytes json_bytes = pal::load(manifest_path_);
            manifest_hash_ = index_.at("manifest_hash").number();
            pal::require(pal::hash(json_bytes.data(), json_bytes.size()) == manifest_hash_,
                         "ACO manifest checksum mismatch");

            global_key_ = VkPipelineBinaryKeyKHR{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            detail_pbin::check_vk(fn_.key(dev_, nullptr, &global_key_), "vkGetPipelineKeyKHR");
            pal::require(global_key_.keySize > 0 && global_key_.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR,
                         "invalid global pipeline binary key size");

            sampled_tmpl_spv_ = pal::load(resolve_template("selftest_tex.spv"));
            views_tmpl_spv_ = pal::load(resolve_template("d3d_pack.spv"));

            if (!cache_base_.empty()) {
                cache_dir_ = u8_to_path(cache_base_) / ("vk-aco-" + target_);
                std::error_code ec;
                std::filesystem::create_directories(cache_dir_, ec);
                if (ec) cache_dir_.clear();
            }
            ready_ = true;
            reason_.clear();
        } catch (const std::exception& e) {
            ready_ = false;
            reason_ = e.what();
        }
    }

    std::string resolve_template(const char* name) const {
        if (!shader_dir_.empty()) {
            const auto cand = u8_to_path(shader_dir_) / name;
            if (std::filesystem::exists(cand)) return path_to_u8(cand);
        }
        const auto fallback = root_.parent_path().parent_path().parent_path() / "shaders" / name;
        return path_to_u8(fallback);
    }

    const pal::Bytes& ensure_template(bool views, VkPipelineLayout layout, VkPipelineCreateFlags pipeline_flags) {
        const size_t slot = views ? 1 : 0;
        std::lock_guard lock(mu_);
        if (!templates_[slot].empty() && template_flags_[slot] == pipeline_flags)
            return templates_[slot];

        const pal::Bytes& spv = views ? views_tmpl_spv_ : sampled_tmpl_spv_;
        pal::require(!spv.empty() && spv.size() % 4 == 0, "invalid template SPIR-V size");

        detail_pbin::CapturedPipe cap(dev_, fn_);
        VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sci.codeSize = spv.size();
        sci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        detail_pbin::check_vk(vkCreateShaderModule(dev_, &sci, nullptr, &cap.mod), "vkCreateShaderModule(template)");

        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        rss.requiredSubgroupSize = 32;

        VkPipelineCreateFlags2CreateInfoKHR f2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
        f2.flags = VkPipelineCreateFlags2KHR(pipeline_flags) | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;

        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &f2};
        ci.flags = pipeline_flags;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &rss,
                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
                    VK_SHADER_STAGE_COMPUTE_BIT, cap.mod, "main", nullptr};
        ci.layout = layout;

        detail_pbin::check_vk(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &cap.pipe),
                              "vkCreateComputePipelines(template capture)");
        cap.captured = true;

        detail_pbin::BinaryList bins(dev_, fn_);
        VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
        bi.pipeline = cap.pipe;
        VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        detail_pbin::check_vk(fn_.create(dev_, &bi, nullptr, &hi), "vkCreatePipelineBinariesKHR(count)");
        pal::require(hi.pipelineBinaryCount == 1, "expected one PAL compute binary in template");
        bins.handles.resize(1);
        hi.pPipelineBinaries = bins.handles.data();
        detail_pbin::check_vk(fn_.create(dev_, &bi, nullptr, &hi), "vkCreatePipelineBinariesKHR(capture)");

        VkPipelineBinaryDataInfoKHR di{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
        di.pipelineBinary = bins.handles[0];
        VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        size_t bytes = 0;
        detail_pbin::check_vk(fn_.data(dev_, &di, &key, &bytes, nullptr), "vkGetPipelineBinaryDataKHR(size)");
        pal::require(bytes > 0 && bytes <= 64 * 1024 * 1024, "captured template size outside limit");
        pal::Bytes elf(bytes);
        detail_pbin::check_vk(fn_.data(dev_, &di, &key, &bytes, elf.data()), "vkGetPipelineBinaryDataKHR(data)");
        elf.resize(bytes);

        const pal::Elf parsed(elf);
        pal::require(env_target_override_ ? pal::is_gfx12(parsed.mach) : (parsed.mach == expected_mach_),
                     "captured Vulkan template ELF mach mismatch");
        (void)pal::xgl_layout(pal::pal_metadata(parsed.data[parsed.find(".note")]), 256);

        templates_[slot] = std::move(elf);
        template_flags_[slot] = pipeline_flags;
        return templates_[slot];
    }

    VkPipeline import_pipeline(VkPipelineLayout layout, VkPipelineCreateFlags pipeline_flags,
                               const VkPipelineBinaryKeyKHR& key_in, const pal::Bytes& blob) const {
        VkPipelineBinaryKeyKHR key = key_in;
        VkPipelineBinaryDataKHR bd{blob.size(), const_cast<uint8_t*>(blob.data())};
        VkPipelineBinaryKeysAndDataKHR kd{1, &key, &bd};
        VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
        bi.pKeysAndDataInfo = &kd;

        detail_pbin::BinaryList imported(dev_, fn_);
        imported.handles.resize(1);
        VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        hi.pipelineBinaryCount = 1;
        hi.pPipelineBinaries = imported.handles.data();
        detail_pbin::check_vk(fn_.create(dev_, &bi, nullptr, &hi), "vkCreatePipelineBinariesKHR(import)");

        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        rss.requiredSubgroupSize = 32;

        VkPipelineBinaryInfoKHR input{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR};
        input.binaryCount = 1;
        input.pPipelineBinaries = imported.handles.data();

        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &input};
        ci.flags = pipeline_flags;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &rss,
                    VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT,
                    VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, "main", nullptr};
        ci.layout = layout;

        VkPipeline pipe = VK_NULL_HANDLE;
        const VkResult r = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        if (r != VK_SUCCESS) {
            if (pipe) vkDestroyPipeline(dev_, pipe, nullptr);
            detail_pbin::check_vk(r, "vkCreateComputePipelines(import)");
        }
        pal::require(pipe != VK_NULL_HANDLE, "imported pipeline handle is null");
        return pipe;
    }

    VkPipelineBinaryKeyKHR derive_binary_key(const pal::Bytes& spliced, uint64_t spv_hash_val) const {
        VkPipelineBinaryKeyKHR key = global_key_;
        const uint64_t h0 = pal::hash(spliced.data(), spliced.size()) ^ spv_hash_val ^ 0x4f4e52564b414331ull;
        uint64_t h = h0 ^ pal::hash(global_key_.key, global_key_.keySize);
        for (uint32_t i = 0; i < key.keySize; ++i) {
            h ^= uint64_t(i) + 1ull;
            h *= 1099511628211ull;
            key.key[i] = uint8_t(h >> (8 * (i % 8)));
        }
        return key;
    }

    uint64_t compute_cache_id(uint64_t spv_hash_val, uint64_t code_hash, uint64_t nrp_hash, uint64_t tmpl_hash,
                              bool views, std::array<uint32_t, 3> wg, VkPipelineCreateFlags pipeline_flags) const {
#ifdef OPENRR_BUILD_STAMP
        static constexpr char build_stamp[] = OPENRR_BUILD_STAMP;
#else
        static constexpr char build_stamp[] = "dev";
#endif
        const uint64_t gk_hash = pal::hash(global_key_.key, global_key_.keySize);
        const uint64_t stamp_hash = pal::hash(build_stamp, sizeof(build_stamp) - 1);
        const uint64_t words[] = {
            0x4f4e52564b414331ull, // "ONRVKAC1" splice/cache version
            gk_hash,
            manifest_hash_,
            spv_hash_val,
            code_hash,
            nrp_hash,
            tmpl_hash,
            (uint64_t(views ? 1 : 0) << 40) | (uint64_t(expected_mach_) << 16) | 32ull,
            (uint64_t(256u) << 32) | uint64_t(pipeline_flags),
            (uint64_t(wg[0]) << 32) | uint64_t(wg[1]),
            uint64_t(wg[2]),
            stamp_hash,
        };
        return pal::hash(words, sizeof(words));
    }

    static bool load_nrb(const std::filesystem::path& file, VkPipelineBinaryKeyKHR& key, pal::Bytes& blob) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        char magic[4]{};
        uint32_t version = 0, key_size = 0;
        uint64_t size = 0, check = 0;
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&key_size), 4);
        if (!in || std::memcmp(magic, "ONRB", 4) != 0 || version != 1 || !key_size ||
            key_size > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR)
            return false;
        key = VkPipelineBinaryKeyKHR{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        key.keySize = key_size;
        in.read(reinterpret_cast<char*>(key.key), VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
        in.read(reinterpret_cast<char*>(&size), 8);
        if (!in || !size || size > 64 * 1024 * 1024) return false;
        blob.resize(size_t(size));
        in.read(reinterpret_cast<char*>(blob.data()), std::streamsize(size));
        in.read(reinterpret_cast<char*>(&check), 8);
        return bool(in) && check == pal::hash(blob.data(), blob.size());
    }

    void save_nrb(const std::filesystem::path& file, const VkPipelineBinaryKeyKHR& key, const pal::Bytes& blob) const {
        const uint64_t seq = tmp_seq_.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
        const unsigned long tid = GetCurrentThreadId();
#else
        const unsigned long pid = 0;
        const unsigned long tid = 0;
#endif
        const std::filesystem::path tmp = u8_to_path(path_to_u8(file) + ".tmp." + std::to_string(pid) + "." +
                                                     std::to_string(tid) + "." + std::to_string(seq));
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out) {
                const uint32_t version = 1, key_size = key.keySize;
                const uint64_t size = blob.size();
                const uint64_t check = pal::hash(blob.data(), blob.size());
                out.write("ONRB", 4);
                out.write(reinterpret_cast<const char*>(&version), 4);
                out.write(reinterpret_cast<const char*>(&key_size), 4);
                out.write(reinterpret_cast<const char*>(key.key), VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
                out.write(reinterpret_cast<const char*>(&size), 8);
                out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(size));
                out.write(reinterpret_cast<const char*>(&check), 8);
                out.flush();
                ok = bool(out);
            }
        }
        if (ok && detail_pbin::atomic_replace_file(tmp, file)) return;
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    detail_pbin::PbinProcs fn_;
    bool ready_ = false;
    bool env_target_override_ = false;
    std::string reason_;
    std::string target_;
    uint32_t expected_mach_ = 0;
    std::string cache_base_, shader_dir_;
    std::filesystem::path root_, cache_dir_;
    std::string manifest_path_;
    uint64_t manifest_hash_ = 0;
    pal::Value index_;
    VkPipelineBinaryKeyKHR global_key_{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    pal::Bytes sampled_tmpl_spv_, views_tmpl_spv_;
    mutable std::mutex mu_;
    std::array<pal::Bytes, 2> templates_;
    std::array<VkPipelineCreateFlags, 2> template_flags_{};
    mutable std::atomic<uint32_t> hits_{0}, misses_{0};
    mutable std::atomic<uint64_t> tmp_seq_{0};
};

}  // namespace onr
