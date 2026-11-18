// SPDX-License-Identifier: MIT
// The Vulkan host backend's device setup (ABI 5, docs/FASE23-VULKAN.md): what our kernels need from a Vulkan game's
// device, added to a cloned VkDeviceCreateInfo/pNext chain at vkCreateDevice without mutating caller memory.
#define NOMINMAX
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "vk_host.hpp"
#include "opennr_rr/vk.hpp"
#include "../../gpu/src/rr_device_chain_sizes.hpp"

namespace onr_host {

namespace {

template <class T> T* chain_find(const void* p, VkStructureType s) {
    for (auto* b = static_cast<const VkBaseInStructure*>(p); b; b = b->pNext)
        if (b->sType == s) return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(b));
    return nullptr;
}

struct ChainAugmentError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Setup {
    VkDeviceCreateInfo out{};
    std::vector<const char*> exts;
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> prios;
    std::vector<std::unique_ptr<uint8_t[]>> owned_nodes;
    std::unordered_set<const void*> cloned_ptrs;
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDevice16BitStorageFeatures s16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDevice8BitStorageFeatures s8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures vmm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    VkPhysicalDeviceShaderAtomicInt64Features a64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures hqr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    VkPhysicalDeviceScalarBlockLayoutFeatures sbl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures ssc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbin{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    uint32_t qfam = 0, qindex = 0;
    bool binary_enabled = false;
    bool saw_unknown = false;
    VkStructureType first_unknown_stype = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    std::string added;

    void clone_pnext_chain(const void* head) {
        if (!head) {
            out.pNext = nullptr;
            return;
        }
        VkBaseOutStructure* prev = nullptr;
        void* new_head = nullptr;
        for (const auto* b = static_cast<const VkBaseInStructure*>(head); b; b = b->pNext) {
            const size_t sz = pnext_struct_size(b->sType);
            if (sz < sizeof(VkBaseOutStructure)) {
                // Attach unknown tail read-only; never write to caller structs beyond this point.
                saw_unknown = true;
                first_unknown_stype = b->sType;
                if (prev) prev->pNext = const_cast<VkBaseOutStructure*>(reinterpret_cast<const VkBaseOutStructure*>(b));
                else new_head = const_cast<void*>(static_cast<const void*>(b));
                break;
            }
            auto buf = std::make_unique<uint8_t[]>(sz);
            std::memcpy(buf.get(), b, sz);
            auto* copy = reinterpret_cast<VkBaseOutStructure*>(buf.get());
            copy->pNext = nullptr;
            cloned_ptrs.insert(copy);
            if (prev) prev->pNext = copy;
            else new_head = copy;
            prev = copy;
            owned_nodes.push_back(std::move(buf));
        }
        out.pNext = new_head;
    }

    void on_node(const void* node, VkBool32& v, const char* name) {
        if (v) return;
        if (!cloned_ptrs.count(node)) {
            throw ChainAugmentError(std::string("cannot safely enable feature '") + name +
                                    "' in pNext chain: preceded by unknown sType=" +
                                    std::to_string(static_cast<int32_t>(first_unknown_stype)));
        }
        v = VK_TRUE;
        added += added.empty() ? name : std::string(" ") + name;
    }

    template <class T> T& ext(T& own) {
        if (T* g = chain_find<T>(out.pNext, own.sType)) return *g;
        own.pNext = const_cast<void*>(out.pNext);
        cloned_ptrs.insert(&own);
        out.pNext = &own;
        return own;
    }

    template <class V, class T> void core_on(VkStructureType vs, VkBool32 V::*vm, T& own, VkBool32 T::*tm, const char* name) {
        if (V* v = chain_find<V>(out.pNext, vs)) on_node(v, v->*vm, name);
        else {
            T& node = ext(own);
            on_node(&node, node.*tm, name);
        }
    }
};

bool has_ext(const std::vector<VkExtensionProperties>& e, const char* n) {
    return std::any_of(e.begin(), e.end(), [&](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, n); });
}

void add_ext_unique(std::vector<const char*>& exts, std::string& added, const char* ext_name) {
    if (std::none_of(exts.begin(), exts.end(), [&](const char* e) { return !std::strcmp(e, ext_name); })) {
        exts.push_back(ext_name);
        added += added.empty() ? ext_name : std::string(" ") + ext_name;
    }
}

}  // namespace

int prepare(VkPhysicalDevice phys, const VkDeviceCreateInfo* in, VkDeviceCreateInfo* out, void** token, std::string& why) {
    *out = *in;
    *token = nullptr;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        why = "the GPU's Vulkan is older than 1.3";
        return -1;
    }
    const onr::TrustedVkIdentity tid = onr::identify_vk_physical_device(phys);
    if (tid.arch == onr::arch::GpuArch::Rdna1 || tid.arch == onr::arch::GpuArch::Rdna2) {
        why = tid.name + ": " + onr::arch::arch_name(tid.arch) + " GPU is not supported (requires RDNA3 or RDNA4)";
        return -1;
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, exts.data());

    const bool has_cm_ext = has_ext(exts, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const bool has_wl_ext = has_ext(exts, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    const bool has_f8_ext = has_ext(exts, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    const bool has_m5_ext = has_ext(exts, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    const bool has_pbin_ext = has_ext(exts, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);

    if (!has_cm_ext) {
        why = std::string("the GPU lacks ") + VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME + " (needs an RDNA4 or RDNA3 GPU)";
        return -1;
    }
    if (!has_wl_ext) {
        why = std::string("the GPU lacks ") + VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME;
        return -1;
    }

    // Require FP8 only on RDNA4; RDNA3 runs FP16-only without VK_EXT_shader_float8.
    const bool need_f8 = (tid.arch == onr::arch::GpuArch::Rdna4);
    if (need_f8 && !has_f8_ext) {
        why = std::string("the RDNA4 GPU lacks ") + VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
        return -1;
    }

    // the EXT buffer device address excludes the core feature the kernels use (the AMD driver does not offer it)
    for (uint32_t i = 0; i < in->enabledExtensionCount; ++i)
        if (!std::strcmp(in->ppEnabledExtensionNames[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            why = "the game enables VK_EXT_buffer_device_address";
            return -1;
        }

    // Query physical device features without mixing Vulkan14Features and Maintenance5FeaturesKHR in one chain.
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &wl};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
                                                    has_f8_ext ? static_cast<void*>(&f8) : static_cast<void*>(&wl)};
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbin_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &cm};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
                                                    has_pbin_ext ? static_cast<void*>(&pbin_feat) : static_cast<void*>(&cm)};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.pNext = has_m5_ext ? static_cast<void*>(&m5_feat)
                           : (has_pbin_ext ? static_cast<void*>(&pbin_feat) : static_cast<void*>(&cm));
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    bool m5_cap = has_m5_ext && m5_feat.maintenance5;
    if (!m5_cap && props.apiVersion >= VK_API_VERSION_1_4) {
        VkPhysicalDeviceVulkan14Features v14_probe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        VkPhysicalDeviceFeatures2 f2_14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v14_probe};
        vkGetPhysicalDeviceFeatures2(phys, &f2_14);
        m5_cap = v14_probe.maintenance5 != VK_FALSE;
    }

    const bool use_f8 = has_f8_ext && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
    if (need_f8 && !use_f8) {
        why = "the RDNA4 GPU lacks shaderFloat8 / shaderFloat8CooperativeMatrix";
        return -1;
    }
    if (!tid.verified_icd && tid.arch == onr::arch::GpuArch::Unknown && !use_f8 && tid.vendor != 0x1002) {
        why = tid.name + ": not an AMD RDNA3/RDNA4 GPU";
        return -1;
    }

    const struct { VkBool32 v; const char* n; } req[] = {
        {f2.features.shaderInt64, "shaderInt64"}, {f2.features.shaderInt16, "shaderInt16"}, {f2.features.shaderFloat64, "shaderFloat64"},
        {v11.storageBuffer16BitAccess, "storageBuffer16BitAccess"}, {v12.storageBuffer8BitAccess, "storageBuffer8BitAccess"},
        {v12.shaderFloat16, "shaderFloat16"}, {v12.shaderInt8, "shaderInt8"}, {v12.bufferDeviceAddress, "bufferDeviceAddress"},
        {v12.vulkanMemoryModel, "vulkanMemoryModel"}, {v12.vulkanMemoryModelDeviceScope, "vulkanMemoryModelDeviceScope"},
        {v12.shaderBufferInt64Atomics, "shaderBufferInt64Atomics"}, {v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {v12.hostQueryReset, "hostQueryReset"}, {v12.scalarBlockLayout, "scalarBlockLayout"},
        {v13.synchronization2, "synchronization2"}, {v13.subgroupSizeControl, "subgroupSizeControl"},
        {v13.computeFullSubgroups, "computeFullSubgroups"}, {cm.cooperativeMatrix, "cooperativeMatrix"},
        {wl.workgroupMemoryExplicitLayout, "workgroupMemoryExplicitLayout"},
        {wl.workgroupMemoryExplicitLayout16BitAccess, "workgroupMemoryExplicitLayout16BitAccess"},
    };
    for (auto& r : req)
        if (!r.v) {
            why = std::string("the GPU lacks the feature ") + r.n + " (needs an RDNA4 or RDNA3 GPU)";
            return -1;
        }

    const bool need_aco = !tid.is_radv && ((onr::arch::aco_target_for_mach(tid.elf_mach) != nullptr) ||
                          (tid.arch == onr::arch::GpuArch::Rdna4) || need_f8 || use_f8);
    if (need_aco && !(has_pbin_ext && pbin_feat.pipelineBinaries && m5_cap)) {
        why = "the RDNA4 GPU / driver lacks VK_KHR_pipeline_binary or maintenance5 required for ACO";
        return -1;
    }

    // Validate caller pNext chain for cycles (by node pointer) and duplicate sTypes where Vulkan requires uniqueness.
    // Vulkan VUID-VkDeviceCreateInfo-sType-unique explicitly permits repeated VkDevicePrivateDataCreateInfo and
    // VkDeviceDeviceMemoryReportCreateInfoEXT structures in VkDeviceCreateInfo::pNext.
    auto allows_duplicate_stype = [](VkStructureType st) {
        switch (st) {
            case VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO:
            case VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT:
                return true;
            default:
                return false;
        }
    };
    std::unordered_set<const void*> seen_nodes;
    std::unordered_set<VkStructureType> in_stypes;
    for (const auto* b = static_cast<const VkBaseInStructure*>(in->pNext); b; b = b->pNext) {
        if (seen_nodes.size() >= 1024 || !seen_nodes.insert(b).second) {
            why = "cyclic or oversized pNext chain in VkDeviceCreateInfo";
            return -1;
        }
        if (!allows_duplicate_stype(b->sType) && !in_stypes.insert(b->sType).second) {
            why = "duplicate sType in VkDeviceCreateInfo::pNext";
            return -1;
        }
    }
    if (in->pEnabledFeatures && in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
        why = "invalid VkDeviceCreateInfo: both pEnabledFeatures and VkPhysicalDeviceFeatures2 are present";
        return -1;
    }
    if (in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) &&
        in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR)) {
        why = "conflicting Vulkan14Features and Maintenance5FeaturesKHR in pNext chain";
        return -1;
    }
    if (in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) &&
        in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)) {
        why = "conflicting Vulkan11Features and 16BitStorageFeatures in pNext chain";
        return -1;
    }
    if (in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
        for (VkStructureType s12_split : {
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
             }) {
            if (in_stypes.count(s12_split)) {
                why = "conflicting Vulkan12Features and split Vulkan 1.2 feature structure in pNext chain";
                return -1;
            }
        }
    }
    if (in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) &&
        (in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) ||
         in_stypes.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES))) {
        why = "conflicting Vulkan13Features and split Vulkan 1.3 feature structure in pNext chain";
        return -1;
    }

    // a queue of our own: a compute family with one to spare, one without graphics first
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    auto st = std::make_unique<Setup>();
    try {
        st->out = *in;
        st->clone_pnext_chain(in->pNext);

        st->queues.assign(in->pQueueCreateInfos, in->pQueueCreateInfos + in->queueCreateInfoCount);
        int pick = -1;
        uint32_t used = 0;
        for (int pass = 0; pass < 2 && pick < 0; ++pass)
            for (uint32_t f = 0; f < nq && pick < 0; ++f) {
                if (!(qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) || (pass == 0 && (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT))) continue;
                uint32_t n = 0;
                bool flagged = false;
                for (auto& q : st->queues)
                    if (q.queueFamilyIndex == f) n += q.queueCount, flagged |= q.flags != 0;
                if (flagged || n >= qf[f].queueCount) continue;
                pick = int(f), used = n;
            }
        if (pick < 0) {
            why = "no queue family has a queue left for the runtime";
            return -1;
        }
        st->qfam = uint32_t(pick), st->qindex = used;
        st->prios.resize(st->queues.size() + 1);
        bool bumped = false;
        for (size_t i = 0; i < st->queues.size(); ++i) {
            auto& q = st->queues[i];
            st->prios[i].assign(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
            if (q.queueFamilyIndex == st->qfam && !bumped) {
                st->prios[i].push_back(*std::min_element(st->prios[i].begin(), st->prios[i].end()));
                ++q.queueCount;
                bumped = true;
            }
            q.pQueuePriorities = st->prios[i].data();
        }
        if (!bumped) {
            st->prios.back() = {0.5f};
            VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex = st->qfam;
            q.queueCount = 1;
            q.pQueuePriorities = st->prios.back().data();
            st->queues.push_back(q);
        }
        st->out.queueCreateInfoCount = uint32_t(st->queues.size());
        st->out.pQueueCreateInfos = st->queues.data();

        st->exts.assign(in->ppEnabledExtensionNames, in->ppEnabledExtensionNames + in->enabledExtensionCount);
        add_ext_unique(st->exts, st->added, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        add_ext_unique(st->exts, st->added, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
        if (use_f8) {
            add_ext_unique(st->exts, st->added, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        }

        const bool use_pbin = !tid.is_radv && has_pbin_ext && pbin_feat.pipelineBinaries && m5_cap;
        const bool has_v14 = chain_find<VkPhysicalDeviceVulkan14Features>(st->out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) != nullptr;
        if (use_pbin) {
            if (!has_v14 && !has_m5_ext) {
                throw ChainAugmentError("VK_KHR_maintenance5 extension is unavailable without Vulkan14Features");
            }
            add_ext_unique(st->exts, st->added, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
            if (has_m5_ext && !has_v14) {
                add_ext_unique(st->exts, st->added, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            }
        }

        st->out.enabledExtensionCount = uint32_t(st->exts.size());
        st->out.ppEnabledExtensionNames = st->exts.data();

        Setup& s = *st;
        if (auto* g2 = chain_find<VkPhysicalDeviceFeatures2>(s.out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
            s.on_node(g2, g2->features.shaderInt64, "shaderInt64");
            s.on_node(g2, g2->features.shaderInt16, "shaderInt16");
            s.on_node(g2, g2->features.shaderFloat64, "shaderFloat64");
        } else {
            if (in->pEnabledFeatures) s.core = *in->pEnabledFeatures;
            s.out.pEnabledFeatures = &s.core;
            s.cloned_ptrs.insert(&s.core);
            s.on_node(&s.core, s.core.shaderInt64, "shaderInt64");
            s.on_node(&s.core, s.core.shaderInt16, "shaderInt16");
            s.on_node(&s.core, s.core.shaderFloat64, "shaderFloat64");
        }
        using V11 = VkPhysicalDeviceVulkan11Features;
        using V12 = VkPhysicalDeviceVulkan12Features;
        using V13 = VkPhysicalDeviceVulkan13Features;
        using V14 = VkPhysicalDeviceVulkan14Features;
        constexpr auto S11 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                       S12 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                       S13 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                       S14 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        s.core_on(S11, &V11::storageBuffer16BitAccess, s.s16, &VkPhysicalDevice16BitStorageFeatures::storageBuffer16BitAccess, "storageBuffer16BitAccess");
        s.core_on(S12, &V12::storageBuffer8BitAccess, s.s8, &VkPhysicalDevice8BitStorageFeatures::storageBuffer8BitAccess, "storageBuffer8BitAccess");
        s.core_on(S12, &V12::shaderFloat16, s.f16, &VkPhysicalDeviceShaderFloat16Int8Features::shaderFloat16, "shaderFloat16");
        s.core_on(S12, &V12::shaderInt8, s.f16, &VkPhysicalDeviceShaderFloat16Int8Features::shaderInt8, "shaderInt8");
        s.core_on(S12, &V12::bufferDeviceAddress, s.bda, &VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress, "bufferDeviceAddress");
        s.core_on(S12, &V12::vulkanMemoryModel, s.vmm, &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModel, "vulkanMemoryModel");
        s.core_on(S12, &V12::vulkanMemoryModelDeviceScope, s.vmm, &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope,
                  "vulkanMemoryModelDeviceScope");
        s.core_on(S12, &V12::shaderBufferInt64Atomics, s.a64, &VkPhysicalDeviceShaderAtomicInt64Features::shaderBufferInt64Atomics, "shaderBufferInt64Atomics");
        s.core_on(S12, &V12::descriptorBindingPartiallyBound, s.di, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound,
                  "descriptorBindingPartiallyBound");
        s.core_on(S12, &V12::hostQueryReset, s.hqr, &VkPhysicalDeviceHostQueryResetFeatures::hostQueryReset, "hostQueryReset");
        s.core_on(S12, &V12::scalarBlockLayout, s.sbl, &VkPhysicalDeviceScalarBlockLayoutFeatures::scalarBlockLayout, "scalarBlockLayout");
        s.core_on(S13, &V13::synchronization2, s.sync2, &VkPhysicalDeviceSynchronization2Features::synchronization2, "synchronization2");
        s.core_on(S13, &V13::subgroupSizeControl, s.ssc, &VkPhysicalDeviceSubgroupSizeControlFeatures::subgroupSizeControl, "subgroupSizeControl");
        s.core_on(S13, &V13::computeFullSubgroups, s.ssc, &VkPhysicalDeviceSubgroupSizeControlFeatures::computeFullSubgroups, "computeFullSubgroups");
        {
            auto& n = s.ext(s.cm);
            s.on_node(&n, n.cooperativeMatrix, "cooperativeMatrix");
        }
        if (use_f8) {
            auto& n = s.ext(s.f8);
            s.on_node(&n, n.shaderFloat8, "shaderFloat8");
            s.on_node(&n, n.shaderFloat8CooperativeMatrix, "shaderFloat8CooperativeMatrix");
        }
        {
            auto& n = s.ext(s.wl);
            s.on_node(&n, n.workgroupMemoryExplicitLayout, "workgroupMemoryExplicitLayout");
            s.on_node(&n, n.workgroupMemoryExplicitLayout16BitAccess, "workgroupMemoryExplicitLayout16BitAccess");
            if (wl.workgroupMemoryExplicitLayout8BitAccess) s.on_node(&n, n.workgroupMemoryExplicitLayout8BitAccess, "workgroupMemoryExplicitLayout8BitAccess");
            if (wl.workgroupMemoryExplicitLayoutScalarBlockLayout)
                s.on_node(&n, n.workgroupMemoryExplicitLayoutScalarBlockLayout, "workgroupMemoryExplicitLayoutScalarBlockLayout");
        }
        if (use_pbin) {
            if (auto* v14 = chain_find<V14>(s.out.pNext, S14)) {
                s.on_node(v14, v14->maintenance5, "maintenance5");
            } else {
                auto& n = s.ext(s.m5);
                s.on_node(&n, n.maintenance5, "maintenance5");
            }
            auto& pb = s.ext(s.pbin);
            s.on_node(&pb, pb.pipelineBinaries, "pipelineBinaries");
            s.binary_enabled = true;
        }

        *out = s.out;
        *token = st.release();
        return 0;
    } catch (const ChainAugmentError& e) {
        *out = *in;
        *token = nullptr;
        why = e.what();
        return -1;
    }
}

int finish(void* token, VkDevice device, uint32_t* family, uint32_t* index, std::string* added) {
    std::unique_ptr<Setup> st(static_cast<Setup*>(token));
    if (!st) return -1;
    if (device) onr::vk_set_device_pipeline_binary(device, st->binary_enabled);
    if (family) *family = st->qfam;
    if (index) *index = st->qindex;
    if (added) *added = st->added;
    return device ? 0 : -1;
}

}  // namespace onr_host
