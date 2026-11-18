// SPDX-License-Identifier: MIT
// vkd3d-proton direct Vulkan interop for Linux Wine/Proton + Mesa RADV (universal package, zero extra .so):
// Queries ID3D12DXVKInteropDevice / ID3D12DXVKInteropDevice1 and ID3D12GraphicsCommandListExt from a
// vkd3d-proton D3D12 device/command list, routing EvaluateFeature straight through OnrRrApi5::RecordVk.
#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>

#include "opennr_rr_api.h"
#include "rr_pe_log.hpp"
#include "rr_pe_vkdevice.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Transcribed verbatim from vkd3d-proton include/vkd3d_command_list_vkd3d_ext.idl and vkd3d_device_vkd3d_ext.idl.
MIDL_INTERFACE("77a86b09-2bea-4801-b89a-37648e104af1")
ID3D12GraphicsCommandListExt : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandle(VkCommandBuffer* pVkCommandBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE LaunchCubinShader(void* handle, UINT32 x, UINT32 y, UINT32 z,
                                                        const void* params, UINT32 param_size) = 0;
};

MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
ID3D12DXVKInteropDevice : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** ppvObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** ppFeatures) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pVkInstance, VkPhysicalDevice* pVkPhysicalDevice,
                                                       VkDevice* pVkDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue* pCommandQueue, VkQueue* pVkQueue,
                                                         UINT32* pVkQueueFamily) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource* pResource, D3D12_RESOURCE_STATES State,
                                                        VkImageLayout* pVkLayout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                            UINT64* pBufferOffset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
};

MIDL_INTERFACE("902d8115-59eb-4406-9518-fe00f991ee65")
ID3D12DXVKInteropDevice1 : public ID3D12DXVKInteropDevice {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                             UINT64* pBufferOffset, VkFormat* pFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                                UINT32 vk_queue_family_index,
                                                                ID3D12CommandQueue** ppQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                                    UINT32 vk_queue_family_index,
                                                                    ID3D12CommandAllocator** ppAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginVkCommandBufferInterop(ID3D12CommandList* pCmdList,
                                                                  VkCommandBuffer* pCommandBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndVkCommandBufferInterop(ID3D12CommandList* pCmdList) = 0;
};

#ifdef __MINGW32__
__CRT_UUID_DECL(ID3D12GraphicsCommandListExt, 0x77a86b09, 0x2bea, 0x4801, 0xb8, 0x9a, 0x37, 0x64, 0x8e, 0x10, 0x4a, 0xf1)
__CRT_UUID_DECL(ID3D12DXVKInteropDevice, 0x39da4e09, 0xbd1c, 0x4198, 0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd)
__CRT_UUID_DECL(ID3D12DXVKInteropDevice1, 0x902d8115, 0x59eb, 0x4406, 0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65)
#endif

extern "C" void rr_vk_use_loader(int yes);
extern "C" const char* rr_vk_source;
extern "C" int rr_vk_missing;
extern "C" const char* rr_vk_first_missing;

namespace openrr::pe::vkd3d {

inline bool is_vkd3d_device(ID3D12Device* dev) {
    if (!dev) return false;
    ID3D12DXVKInteropDevice* p = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice), reinterpret_cast<void**>(&p))) || !p)
        return false;
    p->Release();
    return true;
}

inline VkCommandBuffer peek_vk_cmd(ID3D12GraphicsCommandList* cmd) {
    if (!cmd) return VK_NULL_HANDLE;
    ID3D12GraphicsCommandListExt* ext = nullptr;
    if (FAILED(cmd->QueryInterface(__uuidof(ID3D12GraphicsCommandListExt), reinterpret_cast<void**>(&ext))) || !ext)
        return VK_NULL_HANDLE;
    VkCommandBuffer vk_cb = VK_NULL_HANDLE;
    ext->GetVulkanHandle(&vk_cb);
    ext->Release();
    return vk_cb;
}

struct InteropSession {
    ID3D12Device* d3d_dev = nullptr;
    ID3D12DXVKInteropDevice* interop = nullptr;
    ID3D12DXVKInteropDevice1* interop1 = nullptr;
    ID3D12CommandQueue* own_queue = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue vk_queue = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_3;
    uint32_t queue_family = 0;
    uint32_t queue_index = 0;
    bool supports_d24s8 = false;

    InteropSession() = default;
    InteropSession(const InteropSession&) = delete;
    InteropSession& operator=(const InteropSession&) = delete;
    ~InteropSession() { reset(); }

    void reset() {
        if (own_queue) { own_queue->Release(); own_queue = nullptr; }
        if (interop1) { interop1->Release(); interop1 = nullptr; }
        if (interop) { interop->Release(); interop = nullptr; }
        if (d3d_dev) { d3d_dev->Release(); d3d_dev = nullptr; }
        instance = VK_NULL_HANDLE;
        physical = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        vk_queue = VK_NULL_HANDLE;
        queue_family = 0;
        queue_index = 0;
    }
};

inline bool probe_and_open(ID3D12Device* dev, InteropSession& out, std::string& why) {
    why.clear();
    out.reset();
    if (!dev) return false;

    ID3D12DXVKInteropDevice* interop = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12DXVKInteropDevice), reinterpret_cast<void**>(&interop))) || !interop)
        return false; // Normal Windows D3D12 device, not vkd3d-proton.

    rr_vk_use_loader(0);
    openrr::pe::vulkan_entry_points();
    log("[openrr] vkd3d-proton interop: Vulkan entry points from %s (missing=%d%s%s)",
        rr_vk_source ? rr_vk_source : "none", rr_vk_missing,
        rr_vk_missing ? ", first=" : "", rr_vk_missing && rr_vk_first_missing ? rr_vk_first_missing : "");

    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice vk_dev = VK_NULL_HANDLE;
    if (FAILED(interop->GetVulkanHandles(&inst, &phys, &vk_dev)) || !inst || !phys || !vk_dev) {
        interop->Release();
        why = "vkd3d-proton GetVulkanHandles returned null handles";
        return false;
    }

    // Verify required extensions on vkd3d-proton's VkDevice (added via winevulkan vkCreateDevice hook).
    UINT ext_count = 0;
    bool has_cm = false, has_wl = false, has_f8 = false;
    if (SUCCEEDED(interop->GetDeviceExtensions(&ext_count, nullptr)) && ext_count > 0) {
        std::vector<const char*> exts(ext_count, nullptr);
        if (SUCCEEDED(interop->GetDeviceExtensions(&ext_count, exts.data()))) {
            for (const char* e : exts) {
                if (!e) continue;
                if (std::strcmp(e, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) == 0) has_cm = true;
                else if (std::strcmp(e, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME) == 0) has_wl = true;
                else if (std::strcmp(e, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) == 0) has_f8 = true;
            }
        }
    }

    openrr::pe::vkdevice::DeviceInfo prepared{};
    const bool has_prepared = openrr::pe::vkdevice::get_device_info(vk_dev, &prepared) && prepared.prepared;
    if (!has_cm || !has_wl) {
        interop->Release();
        why = "vkd3d-proton VkDevice lacks VK_KHR_cooperative_matrix/workgroup_memory_explicit_layout; load OptiScaler early (e.g. dxgi.dll) so OpenRR prepares the Vulkan device at vkCreateDevice";
        return false;
    }

    if (!has_f8) {
        uint32_t pext_count = 0;
        bool phys_has_f8 = false;
        if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &pext_count, nullptr) == VK_SUCCESS && pext_count > 0) {
            std::vector<VkExtensionProperties> pexts(pext_count);
            if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &pext_count, pexts.data()) == VK_SUCCESS) {
                for (uint32_t i = 0; i < pext_count; ++i) {
                    if (std::strcmp(pexts[i].extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) == 0) {
                        phys_has_f8 = true;
                        break;
                    }
                }
            }
        }
        if (phys_has_f8) {
            interop->Release();
            why = "vkd3d-proton VkDevice lacks VK_EXT_shader_float8; load OptiScaler early (e.g. dxgi.dll with WINEDLLOVERRIDES=\"dxgi=n,b\") so OpenRR prepares the Vulkan device at vkCreateDevice";
            return false;
        }
    }

    ID3D12DXVKInteropDevice1* interop1 = nullptr;
    if (FAILED(interop->QueryInterface(__uuidof(ID3D12DXVKInteropDevice1), reinterpret_cast<void**>(&interop1)))) {
        interop1 = nullptr;
    }

    uint32_t api_ver = has_prepared ? prepared.api_version : VK_API_VERSION_1_3;
    uint32_t qfam = has_prepared ? prepared.queue_family : 0;
    uint32_t qidx = has_prepared ? prepared.queue_index : 0;
    VkQueue vk_q = VK_NULL_HANDLE;
    ID3D12CommandQueue* own_q = nullptr;

    if (!has_prepared) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&own_q))) || !own_q ||
            FAILED(interop->GetVulkanQueueInfo(own_q, &vk_q, &qfam)) || !vk_q) {
            if (own_q) own_q->Release();
            if (interop1) interop1->Release();
            interop->Release();
            why = "vkd3d-proton GetVulkanQueueInfo failed";
            return false;
        }
        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &nf, qfp.data());
        const uint32_t qc = qfam < qfp.size() ? qfp[qfam].queueCount : 1u;
        for (uint32_t i = 0; i < qc; ++i) {
            VkQueue cand = VK_NULL_HANDLE;
            vkGetDeviceQueue(vk_dev, qfam, i, &cand);
            if (cand == vk_q) {
                qidx = i;
                break;
            }
        }
    }

    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);

    dev->AddRef();
    out.d3d_dev = dev;
    out.interop = interop;
    out.interop1 = interop1;
    out.own_queue = own_q;
    out.instance = inst;
    out.physical = phys;
    out.device = vk_dev;
    out.vk_queue = vk_q;
    out.api_version = api_ver;
    out.queue_family = qfam;
    out.queue_index = qidx;
    out.supports_d24s8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    log("[openrr] vkd3d-proton direct Vulkan interop active (prepared=%d, fp8_ext=%d, interop1=%d, qfam=%u, qidx=%u)",
        int(has_prepared), int(has_f8), int(interop1 != nullptr), qfam, qidx);
    return true;
}

inline VkFormat dxgi_to_vk_format(DXGI_FORMAT fmt, uint32_t d3d_flags, bool supports_d24s8) {
    switch (fmt) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return supports_d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        case DXGI_FORMAT_D32_FLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case DXGI_FORMAT_R32_TYPELESS:
            return (d3d_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_D16_UNORM:
            return VK_FORMAT_D16_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:
            return (d3d_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? VK_FORMAT_D16_UNORM : VK_FORMAT_R16_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
            return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return VK_FORMAT_R8G8B8A8_SNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_TYPELESS:
            return VK_FORMAT_R16G16_SFLOAT;
        case DXGI_FORMAT_R16G16_SNORM:
            return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_UNORM:
            return VK_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_TYPELESS:
            return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R16_FLOAT:
            return VK_FORMAT_R16_SFLOAT;
        case DXGI_FORMAT_R16_UNORM:
            return VK_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_TYPELESS:
            return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

inline bool fill_vk_image(InteropSession& s, const OnrRrTexture& src, OnrRrVkImage& out_img, OnrRrTexture& dst_tex) {
    dst_tex = {};
    if (!src.resource) return true;
    auto* res = static_cast<ID3D12Resource*>(src.resource);
    UINT64 vk_handle = 0, offset = 0;
    VkFormat created_fmt = VK_FORMAT_UNDEFINED;
    HRESULT hr = E_FAIL;
    if (s.interop1) {
        hr = s.interop1->GetVulkanResourceInfo1(res, &vk_handle, &offset, &created_fmt);
    }
    if (FAILED(hr) || !vk_handle) {
        hr = s.interop->GetVulkanResourceInfo(res, &vk_handle, &offset);
    }
    if (FAILED(hr) || !vk_handle) return false;

    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    s.interop->GetVulkanImageLayout(res, D3D12_RESOURCE_STATES(src.state), &layout);
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    VkFormat vk_fmt = dxgi_to_vk_format(desc.Format, desc.Flags, s.supports_d24s8);
    if (vk_fmt == VK_FORMAT_UNDEFINED) vk_fmt = created_fmt;
    if (vk_fmt == VK_FORMAT_UNDEFINED) return false;

    uint32_t usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    out_img = {};
    out_img.image = vk_handle;
    out_img.view = 0;
    out_img.format = uint32_t(vk_fmt);
    out_img.width = uint32_t(desc.Width);
    out_img.height = desc.Height;
    out_img.mip = 0;
    out_img.layer = 0;
    out_img.usage = usage;

    dst_tex.resource = &out_img;
    dst_tex.state = uint32_t(layout);
    dst_tex.x = src.x;
    dst_tex.y = src.y;
    return true;
}

struct ScopedCmdInterop {
    InteropSession& s;
    ID3D12GraphicsCommandList* list = nullptr;
    VkCommandBuffer vk_cmd = VK_NULL_HANDLE;
    bool ended_by_interop1 = false;

    ScopedCmdInterop(InteropSession& sess, ID3D12GraphicsCommandList* cl) : s(sess), list(cl) {
        if (!cl) return;
        if (s.interop1 && SUCCEEDED(s.interop1->BeginVkCommandBufferInterop(cl, &vk_cmd)) && vk_cmd) {
            ended_by_interop1 = true;
            return;
        }
        vk_cmd = peek_vk_cmd(cl);
    }
    ScopedCmdInterop(const ScopedCmdInterop&) = delete;
    ScopedCmdInterop& operator=(const ScopedCmdInterop&) = delete;
    ~ScopedCmdInterop() {
        if (ended_by_interop1 && s.interop1 && list) {
            s.interop1->EndVkCommandBufferInterop(list);
        }
    }
};

} // namespace openrr::pe::vkd3d
