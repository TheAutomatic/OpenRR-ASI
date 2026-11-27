// openrr-amd.asi: Native OptiScaler plugin & Ray Reconstruction (DLSSD) provider for AMD GPUs.
// Shell adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_path_win.hpp"
#include "rr_ngx_abi.hpp"
#include "rr_opti_plugin.hpp"
#include "rr_pe_config.hpp"
#include "rr_pe_control.hpp"
#include "rr_pe_crash.hpp"
#include "rr_pe_d3d12_hook_policy.hpp"
#include "rr_pe_d3d12_direct_hooks.hpp"
#include "rr_pe_d3d12split.hpp"
#include "rr_d3d12_record.hpp"
#include "rr_data_paths.hpp"
#include "rr_pe_log.hpp"
#include "rr_pe_nvapi_compat.hpp"
#include "rr_pe_vkdevice.hpp"
#include "rr_pe_vkd3d_interop.hpp"
#include "rr_pe_vkreset.hpp"
#include "rr_pe_watchdog.hpp"
#include "camera_and_sl.hpp"
#include "opennr_rr_api.h"
#include "opennr_rr/vk.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>
#include "MinHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef OPENRR_BUILD_STAMP
#define OPENRR_BUILD_STAMP "dev"
#endif

namespace {

using openrr::pe::log;

HMODULE g_self = nullptr;

std::wstring wide_from_utf8(const std::string& s) {
    return openrr::u8::wide(s);
}

std::string utf8_from_wide(const std::wstring& w) {
    return openrr::u8::narrow(w);
}

// ---- Deferred post-DllMain initialization (Item #8) ----
std::once_flag g_deferred_init_once;
void ensure_deferred_init() {
    std::call_once(g_deferred_init_once, [] {
        openrr::pe::restore_ini(openrr::pe::module_folder());
        openrr::pe::control::asi_facts(OPENRR_BUILD_STAMP);
        openrr::pe::watchdog::start();
    });
}

// ---- Coexistence detection (dlssnr-amd*.asi, Item #7) ----
std::atomic<int> g_coexist_mochi{-1};

bool probe_coexist_mochi() {
    const bool found = GetModuleHandleW(L"dlssnr-amd.asi") != nullptr ||
                       GetModuleHandleW(L"dlssnr-amd-loadlate.asi") != nullptr;
    g_coexist_mochi.store(found ? 1 : 0, std::memory_order_release);
    return found;
}

bool coexist_mochi_detected() {
    const int c = g_coexist_mochi.load(std::memory_order_acquire);
    if (c >= 0) return c == 1;
    return probe_coexist_mochi();
}

bool split_allowed_by_config() {
    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.coexist_split == "off") return false;
    if (coexist_mochi_detected()) return false;
    return true;
}

// ---- D3D12 MinHook compute-state tracker for Direct mode (Item #1) & Allocator Reset (Item #3) ----
bool in_hookable_d3d12_module(const void* fn, std::string* out_mod = nullptr) {
    return openrr::pe::is_hookable_d3d12_target(fn, g_self, out_mod);
}

struct DirectRootArgs {
    ID3D12RootSignature* rs = nullptr;
    bool rs_set = false;
    uint64_t mask = 0;
    uint8_t kind[64]{};   // 1 table, 2 CBV, 3 SRV, 4 UAV
    uint64_t value[64]{};
    uint64_t const_param_mask = 0;
    uint64_t const_dword_mask[64]{};
    std::unique_ptr<std::array<uint32_t, 64>> const_vals[64]{};

    void clear_bindings() {
        mask = 0;
        const_param_mask = 0;
    }

    void set_rs(ID3D12RootSignature* r) {
        if (!rs_set || r != rs) clear_bindings();
        rs = r;
        rs_set = true;
    }

    void set(UINT index, uint8_t k, uint64_t v) {
        if (index >= 64) return;
        mask |= (1ull << index);
        kind[index] = k;
        value[index] = v;
    }

    void constant(UINT index, UINT offset, uint32_t v) {
        if (index >= 64 || offset >= 64) return;
        if (!const_vals[index]) const_vals[index] = std::make_unique<std::array<uint32_t, 64>>();
        if (!(const_param_mask & (1ull << index))) {
            const_dword_mask[index] = 0;
            const_param_mask |= (1ull << index);
        }
        (*const_vals[index])[offset] = v;
        const_dword_mask[index] |= (1ull << offset);
    }

    void clear_constant_range(UINT index, UINT offset, UINT count) {
        if (index >= 64 || offset >= 64 || count == 0) return;
        for (UINT i = 0; i < count && (offset + i) < 64; ++i) {
            constant(index, offset + i, 0u);
        }
    }

    void clear_root_view(UINT index, uint8_t k) {
        if (index >= 64) return;
        if ((mask & (1ull << index)) && (k == 0 || kind[index] == k)) {
            mask &= ~(1ull << index);
        }
    }
};

struct DirectCmdSigInfo {
    uint8_t pipeline_kind = 0; // 1 = graphics, 2 = compute
    uint64_t view_clear_mask = 0;
    uint8_t view_clear_kind[64]{};
    uint64_t const_clear_param_mask = 0;
    uint64_t const_clear_dword_mask[64]{};
};

void apply_cmd_sig_to_root(DirectRootArgs& r, const DirectCmdSigInfo* sig) {
    if (!sig) return;
    for (UINT i = 0; i < 64; ++i) {
        if (sig->view_clear_mask & (1ull << i)) {
            r.clear_root_view(i, sig->view_clear_kind[i]);
        }
        if (sig->const_clear_param_mask & (1ull << i)) {
            const uint64_t dm = sig->const_clear_dword_mask[i];
            for (UINT off = 0; off < 64; ++off) {
                if (dm & (1ull << off)) r.constant(i, off, 0u);
            }
        }
    }
}

struct ObservedTextureBarrier {
    ID3D12Resource* resource;
    bool enhanced;
    UINT before, after;
    UINT access_before, access_after;
    UINT flags;
    D3D12_BARRIER_SUBRESOURCE_RANGE range;
};

struct DirectListState {
    // Diagnostic history only. A command list can inherit states from earlier submissions.
    std::vector<ObservedTextureBarrier> barriers;
    size_t barrier_cursor = 0;

    void note_barrier(const ObservedTextureBarrier& b) {
        constexpr size_t capacity = 128;
        if (barriers.size() < capacity) barriers.push_back(b);
        else barriers[barrier_cursor] = b;
        barrier_cursor = (barrier_cursor + 1) % capacity;
    }

    void clear_barriers() {
        barriers.clear();
        barrier_cursor = 0;
    }

    const ObservedTextureBarrier* last_barrier(const void* resource) const {
        for (size_t i = 0; i < barriers.size(); ++i) {
            const size_t pos = (barrier_cursor + barriers.size() - i - 1) % barriers.size();
            if (barriers[pos].resource == resource) return &barriers[pos];
        }
        return nullptr;
    }
    bool valid = false;
    bool unknown = false;
    UINT heap_count = 0;
    ID3D12DescriptorHeap* heaps[2]{};
    DirectRootArgs compute_root{};
    ID3D12PipelineState* pso = nullptr;
    bool pso_set = false;
    ID3D12StateObject* state_object = nullptr;
    bool so_set = false;
    bool so_last = false;

    void reset(ID3D12PipelineState* initial) {
        valid = true;
        unknown = false;
        heap_count = 0;
        heaps[0] = heaps[1] = nullptr;
        compute_root.rs = nullptr;
        compute_root.rs_set = false;
        compute_root.clear_bindings();
        pso = initial;
        pso_set = (initial != nullptr);
        state_object = nullptr;
        so_set = false;
        so_last = false;
    }

    void clear_state(ID3D12PipelineState* initial) {
        const bool v = valid;
        const bool u = unknown;
        reset(initial);
        valid = v;
        unknown = u;
    }
};

struct DirectStateShard {
    std::mutex mutex;
    std::unordered_map<void*, DirectListState*> map;
};
DirectStateShard g_direct_shards[16];
thread_local void* t_direct_last_key = nullptr;
thread_local DirectListState* t_direct_last_state = nullptr;

// Entries are never freed so thread_local pointers on other threads never dangle.
DirectListState* direct_state_of(void* list) {
    if (!list) return nullptr;
    if (list == t_direct_last_key && t_direct_last_state) return t_direct_last_state;
    const size_t idx = (reinterpret_cast<uintptr_t>(list) >> 4) & 15u;
    DirectStateShard& shard = g_direct_shards[idx];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& ptr = shard.map[list];
    if (!ptr) ptr = new DirectListState();
    t_direct_last_key = list;
    t_direct_last_state = ptr;
    return ptr;
}

void direct_forget_list(void* list) {
    if (!list) return;
    if (t_direct_last_key == list) {
        t_direct_last_key = nullptr;
        t_direct_last_state = nullptr;
    }
    const size_t idx = (reinterpret_cast<uintptr_t>(list) >> 4) & 15u;
    DirectStateShard& shard = g_direct_shards[idx];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(list);
    if (it != shard.map.end() && it->second) {
        it->second->reset(nullptr);
        it->second->clear_barriers();
        it->second->valid = false;
    }
}

std::mutex g_direct_cmd_sig_mutex;
std::unordered_map<void*, DirectCmdSigInfo> g_direct_cmd_sigs;

void register_direct_cmd_sig(void* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc) {
    if (!sig || !desc) return;
    DirectCmdSigInfo info{};
    if (desc->pArgumentDescs && desc->NumArgumentDescs > 0) {
        for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
            const D3D12_INDIRECT_ARGUMENT_DESC& a = desc->pArgumentDescs[i];
            switch (static_cast<uint32_t>(a.Type)) {
                case 0: // D3D12_INDIRECT_ARGUMENT_TYPE_DRAW
                case 1: // D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                case 3: // D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW
                case 4: // D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW
                case 10: // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
                    info.pipeline_kind = 1;
                    break;
                case 2: // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH
                case 9: // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS
                    info.pipeline_kind = 2;
                    break;
                case 5: // D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT
                case 11: { // D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT
                    const UINT idx = a.Constant.RootParameterIndex;
                    const UINT off = a.Constant.DestOffsetIn32BitValues;
                    const UINT cnt = (static_cast<uint32_t>(a.Type) == 11) ? 1u : a.Constant.Num32BitValuesToSet;
                    if (idx < 64 && off < 64 && cnt > 0) {
                        info.const_clear_param_mask |= (1ull << idx);
                        for (UINT k = 0; k < cnt && (off + k) < 64; ++k) {
                            info.const_clear_dword_mask[idx] |= (1ull << (off + k));
                        }
                    }
                    break;
                }
                case 6: { // D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW
                    const UINT idx = a.ConstantBufferView.RootParameterIndex;
                    if (idx < 64) {
                        info.view_clear_mask |= (1ull << idx);
                        info.view_clear_kind[idx] = 2;
                    }
                    break;
                }
                case 7: { // D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW
                    const UINT idx = a.ShaderResourceView.RootParameterIndex;
                    if (idx < 64) {
                        info.view_clear_mask |= (1ull << idx);
                        info.view_clear_kind[idx] = 3;
                    }
                    break;
                }
                case 8: { // D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW
                    const UINT idx = a.UnorderedAccessView.RootParameterIndex;
                    if (idx < 64) {
                        info.view_clear_mask |= (1ull << idx);
                        info.view_clear_kind[idx] = 4;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_direct_cmd_sig_mutex);
    g_direct_cmd_sigs[sig] = info;
}

void apply_direct_execute_indirect(DirectListState* s, void* sig) {
    if (!s) return;
    DirectCmdSigInfo info{};
    bool found = false;
    if (sig) {
        std::lock_guard<std::mutex> lock(g_direct_cmd_sig_mutex);
        auto it = g_direct_cmd_sigs.find(sig);
        if (it != g_direct_cmd_sigs.end()) {
            info = it->second;
            found = true;
        }
    }
    if (!found) {
        s->unknown = true;
        return;
    }
    if (info.pipeline_kind == 2) {
        apply_cmd_sig_to_root(s->compute_root, &info);
    }
}

std::mutex g_direct_hook_install_mutex;
std::atomic<bool> g_direct_hooks_installed{false};
std::atomic<bool> g_direct_hooks_ok{false};
std::vector<void*> g_known_direct_hook_fns;
openrr::pe::EntryHookRegistry<8> g_cmd_sig_hooks;
openrr::pe::EntryHookRegistry<4> g_ca_reset_hooks;
openrr::pe::EntryHookRegistry<4> g_ca_release_hooks;
std::atomic<void**> g_alloc_probed_dev_vts[8]{};
std::atomic<size_t> g_alloc_probed_count{0};
thread_local int t_in_openrr_record = 0;
thread_local int t_in_internal_vk = 0;

struct InternalVkScope {
    InternalVkScope() { ++t_in_internal_vk; }
    ~InternalVkScope() { --t_in_internal_vk; }
};

const GUID kIID_List4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
const GUID kIID_Dev4  = {0xe865df17, 0xa9ee, 0x46f9, {0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5}};

using PFN_CL_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_CL_ClearState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_CL_SetPipelineState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_CL_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_CL_SetDescriptorHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_CL_SetComputeRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_CL_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_CL_SetComputeRoot32BitConstant = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_CL_SetComputeRoot32BitConstants = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_CL_SetComputeRootView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_CL_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_CL_SetPipelineState1 = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12StateObject*);
using PFN_Dev_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using PFN_Dev_CreateCommandSignature = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
using PFN_Dev4_CreateCommandList1 = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3D12_COMMAND_LIST_TYPE, UINT, REFIID, void**);
using PFN_CA_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
using PFN_COM_Release = ULONG(STDMETHODCALLTYPE*)(void*);

PFN_CL_Reset real_cl_reset = nullptr;
PFN_CL_ClearState real_cl_clear_state = nullptr;
PFN_CL_SetPipelineState real_cl_set_pso = nullptr;
PFN_CL_ExecuteBundle real_cl_exec_bundle = nullptr;
PFN_CL_SetDescriptorHeaps real_cl_set_heaps = nullptr;
PFN_CL_SetComputeRootSignature real_cl_set_compute_rs = nullptr;
PFN_CL_SetComputeRootDescriptorTable real_cl_set_compute_table = nullptr;
PFN_CL_SetComputeRoot32BitConstant real_cl_set_compute_const = nullptr;
PFN_CL_SetComputeRoot32BitConstants real_cl_set_compute_consts = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_cbv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_srv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_uav = nullptr;
PFN_CL_ExecuteIndirect real_cl_exec_indirect = nullptr;
PFN_CL_SetPipelineState1 real_cl_set_pso1 = nullptr;
PFN_Dev_CreateCommandList real_dev_create_cl = nullptr;
PFN_Dev4_CreateCommandList1 real_dev4_create_cl1 = nullptr;
PFN_COM_Release real_cl_release = nullptr;

HRESULT hk_ca_reset_impl(size_t slot, ID3D12CommandAllocator* alloc) {
    auto orig = reinterpret_cast<PFN_CA_Reset>(g_ca_reset_hooks.slots[slot].orig);
    const HRESULT hr = orig(alloc);
    if (SUCCEEDED(hr) && alloc) OnrRrNotifyD3d12AllocatorReset(alloc);
    return hr;
}

template <size_t Slot>
HRESULT STDMETHODCALLTYPE hk_ca_reset_slot(ID3D12CommandAllocator* alloc) {
    return hk_ca_reset_impl(Slot, alloc);
}

void* const kCaResetDetours[4] = {
    reinterpret_cast<void*>(&hk_ca_reset_slot<0>),
    reinterpret_cast<void*>(&hk_ca_reset_slot<1>),
    reinterpret_cast<void*>(&hk_ca_reset_slot<2>),
    reinterpret_cast<void*>(&hk_ca_reset_slot<3>),
};

ULONG hk_ca_release_impl(size_t slot, void* alloc) {
    auto orig = reinterpret_cast<PFN_COM_Release>(g_ca_release_hooks.slots[slot].orig);
    const ULONG left = orig(alloc);
    if (left == 0 && alloc) {
        OnrRrNotifyD3d12AllocatorReleased(alloc);
    }
    return left;
}

template <size_t Slot>
ULONG STDMETHODCALLTYPE hk_ca_release_slot(void* alloc) {
    return hk_ca_release_impl(Slot, alloc);
}

void* const kCaReleaseDetours[4] = {
    reinterpret_cast<void*>(&hk_ca_release_slot<0>),
    reinterpret_cast<void*>(&hk_ca_release_slot<1>),
    reinterpret_cast<void*>(&hk_ca_release_slot<2>),
    reinterpret_cast<void*>(&hk_ca_release_slot<3>),
};

ULONG STDMETHODCALLTYPE hk_cl_release(void* cl) {
    const ULONG left = real_cl_release(cl);
    if (left == 0 && cl) {
        OnrRrNotifyD3d12ListReleased(cl);
        direct_forget_list(cl);
    }
    return left;
}

HRESULT STDMETHODCALLTYPE hk_cl_reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
    const HRESULT hr = real_cl_reset(cl, alloc, pso);
    if (SUCCEEDED(hr) && cl && !t_in_openrr_record) {
        OnrRrNotifyD3d12ListReset(cl, alloc);
        if (VkCommandBuffer vk_cb = openrr::pe::vkd3d::peek_vk_cmd(cl)) {
            OnrRrNotifyVkCommandBufferReset(vk_cb);
        }
        DirectListState* state = direct_state_of(cl);
        state->reset(pso);
        state->clear_barriers();
    }
    return hr;
}

void STDMETHODCALLTYPE hk_cl_clear_state(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->clear_state(pso);
    }
    real_cl_clear_state(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (cl && !t_in_openrr_record) {
        DirectListState* s = direct_state_of(cl);
        s->pso = pso;
        s->pso_set = true;
        s->so_last = false;
    }
    real_cl_set_pso(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso1(ID3D12GraphicsCommandList* cl, ID3D12StateObject* so) {
    if (cl && !t_in_openrr_record) {
        DirectListState* s = direct_state_of(cl);
        s->state_object = so;
        s->so_set = true;
        s->so_last = true;
    }
    real_cl_set_pso1(cl, so);
}

void STDMETHODCALLTYPE hk_cl_exec_bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* bundle) {
    if (cl && !t_in_openrr_record) {
        DirectListState* s = direct_state_of(cl);
        const DirectListState* bs = (bundle && bundle != cl) ? direct_state_of(bundle) : nullptr;
        // Point the thread-local cache back at the primary command list (Item #1).
        if (bundle && bundle != cl) direct_state_of(cl);
        if (bs && bs->valid) {
            if (bs->unknown) {
                s->unknown = true;
            } else {
                const DirectRootArgs& br = bs->compute_root;
                if (br.rs_set) s->compute_root.set_rs(br.rs);
                for (UINT i = 0; i < 64; ++i) {
                    if (br.mask & (1ull << i)) s->compute_root.set(i, br.kind[i], br.value[i]);
                    if ((br.const_param_mask & (1ull << i)) && br.const_vals[i]) {
                        uint64_t dm = br.const_dword_mask[i];
                        for (UINT off = 0; off < 64; ++off)
                            if (dm & (1ull << off)) s->compute_root.constant(i, off, (*br.const_vals[i])[off]);
                    }
                }
                if (bs->pso_set) { s->pso = bs->pso; s->pso_set = true; s->so_last = false; }
                if (bs->so_set) { s->state_object = bs->state_object; s->so_set = true; s->so_last = bs->so_last; }
            }
        }
    }
    real_cl_exec_bundle(cl, bundle);
}

void STDMETHODCALLTYPE hk_cl_set_heaps(ID3D12GraphicsCommandList* cl, UINT count, ID3D12DescriptorHeap* const* heaps) {
    if (cl && !t_in_openrr_record) {
        DirectListState* s = direct_state_of(cl);
        const UINT n = std::min<UINT>(count, 2u);
        bool same = (n == s->heap_count);
        for (UINT i = 0; same && i < n; ++i) same = (s->heaps[i] == (heaps ? heaps[i] : nullptr));
        if (!same) {
            for (UINT i = 0; i < 64; ++i) {
                if ((s->compute_root.mask & (1ull << i)) && s->compute_root.kind[i] == 1)
                    s->compute_root.mask &= ~(1ull << i);
            }
        }
        s->heap_count = n;
        for (UINT i = 0; i < n; ++i) s->heaps[i] = heaps ? heaps[i] : nullptr;
    }
    real_cl_set_heaps(cl, count, heaps);
}

using LegacyBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using EnhancedBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);
LegacyBarrierFn real_cl_barrier = nullptr;
EnhancedBarrierFn real_cl_barrier7 = nullptr;

void STDMETHODCALLTYPE hk_cl_barrier(ID3D12GraphicsCommandList* cl, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (cl && barriers && !t_in_openrr_record) {
        auto* state = direct_state_of(cl);
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto& t = b.Transition;
                state->note_barrier({t.pResource, false, UINT(t.StateBefore), UINT(t.StateAfter),
                                     0, 0, UINT(b.Flags), {t.Subresource}});
            } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                state->clear_barriers();
            }
        }
    }
    real_cl_barrier(cl, count, barriers);
}

void STDMETHODCALLTYPE hk_cl_barrier7(ID3D12GraphicsCommandList7* cl, UINT32 count, const D3D12_BARRIER_GROUP* groups) {
    if (cl && groups && !t_in_openrr_record) {
        auto* state = direct_state_of(static_cast<ID3D12GraphicsCommandList*>(cl));
        for (UINT32 i = 0; i < count; ++i) {
            const auto& group = groups[i];
            if (group.Type != D3D12_BARRIER_TYPE_TEXTURE || !group.pTextureBarriers) continue;
            for (UINT32 j = 0; j < group.NumBarriers; ++j) {
                const auto& b = group.pTextureBarriers[j];
                state->note_barrier({b.pResource, true, UINT(b.LayoutBefore), UINT(b.LayoutAfter),
                                     UINT(b.AccessBefore), UINT(b.AccessAfter), UINT(b.Flags), b.Subresources});
            }
        }
    }
    real_cl_barrier7(cl, count, groups);
}

void STDMETHODCALLTYPE hk_cl_set_compute_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.set_rs(rs);
    }
    real_cl_set_compute_rs(cl, rs);
}

void STDMETHODCALLTYPE hk_cl_set_compute_table(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.set(index, 1, h.ptr);
    }
    real_cl_set_compute_table(cl, index, h);
}

void STDMETHODCALLTYPE hk_cl_set_compute_const(ID3D12GraphicsCommandList* cl, UINT index, UINT val, UINT offset) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.constant(index, offset, val);
    }
    real_cl_set_compute_const(cl, index, val, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_consts(ID3D12GraphicsCommandList* cl, UINT index, UINT count, const void* data, UINT offset) {
    if (cl && data && !t_in_openrr_record) {
        DirectRootArgs& r = direct_state_of(cl)->compute_root;
        const auto* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count; ++i) r.constant(index, offset + i, v[i]);
    }
    real_cl_set_compute_consts(cl, index, count, data, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_cbv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.set(index, 2, va);
    }
    real_cl_set_compute_cbv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_srv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.set(index, 3, va);
    }
    real_cl_set_compute_srv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_uav(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (cl && !t_in_openrr_record) {
        direct_state_of(cl)->compute_root.set(index, 4, va);
    }
    real_cl_set_compute_uav(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_exec_indirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                                           UINT max_count, ID3D12Resource* arg_buf, UINT64 arg_offset,
                                           ID3D12Resource* count_buf, UINT64 count_offset) {
    if (cl && !t_in_openrr_record) {
        apply_direct_execute_indirect(direct_state_of(cl), sig);
    }
    real_cl_exec_indirect(cl, sig, max_count, arg_buf, arg_offset, count_buf, count_offset);
}

HRESULT hk_dev_create_cmd_sig_impl(size_t slot, ID3D12Device* dev, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                   ID3D12RootSignature* rs, REFIID riid, void** out) {
    auto orig = reinterpret_cast<PFN_Dev_CreateCommandSignature>(g_cmd_sig_hooks.slots[slot].orig);
    const HRESULT hr = orig(dev, desc, rs, riid, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        ID3D12CommandSignature* sig = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&sig))) && sig) {
            register_direct_cmd_sig(sig, desc);
            sig->Release();
        } else {
            register_direct_cmd_sig(*out, desc);
        }
    }
    return hr;
}

template <size_t Slot>
HRESULT STDMETHODCALLTYPE hk_dev_create_cmd_sig_slot(ID3D12Device* dev, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                     ID3D12RootSignature* rs, REFIID riid, void** out) {
    return hk_dev_create_cmd_sig_impl(Slot, dev, desc, rs, riid, out);
}

void* const kCmdSigDetours[8] = {
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<0>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<1>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<2>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<3>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<4>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<5>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<6>),
    reinterpret_cast<void*>(&hk_dev_create_cmd_sig_slot<7>),
};

HRESULT STDMETHODCALLTYPE hk_dev_create_cl(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                           ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out) {
    const HRESULT hr = real_dev_create_cl(dev, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && !t_in_openrr_record) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            OnrRrNotifyD3d12ListReset(list, alloc);
            DirectListState* state = direct_state_of(list);
            state->reset(pso);
            state->clear_barriers();
            list->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_dev4_create_cl1(void* dev4, UINT mask, D3D12_COMMAND_LIST_TYPE type, UINT flags,
                                             REFIID riid, void** out) {
    const HRESULT hr = real_dev4_create_cl1(dev4, mask, type, flags, riid, out);
    if (SUCCEEDED(hr) && out && *out && !t_in_openrr_record) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            OnrRrNotifyD3d12ListReset(list, nullptr);
            DirectListState* state = direct_state_of(list);
            state->reset(nullptr);
            state->clear_barriers();
            list->Release();
        }
    }
    return hr;
}

bool ensure_cmd_sig_hook(ID3D12Device* dev) {
    if (!dev) return false;
    void** dvt = *reinterpret_cast<void***>(dev);
    void* fn = dvt[41];
    if (g_cmd_sig_hooks.is_covered_lockfree(fn)) return true;

    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    auto pred = [](const void* target, std::string* out_mod) {
        return in_hookable_d3d12_module(target, out_mod);
    };
    std::string mod;
    MH_STATUS mh = MH_OK;
    const auto out = openrr::pe::ensure_entry_hook_locked(
        g_cmd_sig_hooks, fn, kCmdSigDetours, g_known_direct_hook_fns, pred, &mod, &mh);
    switch (out) {
        case openrr::pe::EntryHookOutcome::AlreadyCovered:
        case openrr::pe::EntryHookOutcome::Installed:
            return true;
        case openrr::pe::EntryHookOutcome::AlreadyFailed:
            return false;
        case openrr::pe::EntryHookOutcome::RefusedPolicy:
            log("[openrr] CreateCommandSignature slot 41 refused (%s)", mod.c_str());
            return false;
        case openrr::pe::EntryHookOutcome::DuplicateOtherHook:
            log("[openrr] CreateCommandSignature slot 41 duplicates another hook (%s)", mod.c_str());
            return false;
        case openrr::pe::EntryHookOutcome::SlotsFull:
            log("[openrr] CreateCommandSignature hook slots full (%s)", mod.c_str());
            return false;
        case openrr::pe::EntryHookOutcome::CreateFailed:
            log("[openrr] CreateCommandSignature hook create failed in %s (status=%d)", mod.c_str(), int(mh));
            return false;
        case openrr::pe::EntryHookOutcome::EnableFailed:
            log("[openrr] CreateCommandSignature hook enable failed in %s (status=%d)", mod.c_str(), int(mh));
            return false;
    }
    return false;
}

bool ensure_allocator_reset_hook(ID3D12Device* dev) {
    const bool cmd_sig_ok = ensure_cmd_sig_hook(dev);
    if (!dev) return false;

    void** dvt = *reinterpret_cast<void***>(dev);
    const size_t probed_n = g_alloc_probed_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < probed_n; ++i) {
        if (g_alloc_probed_dev_vts[i].load(std::memory_order_relaxed) == dvt) return cmd_sig_ok;
    }

    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    const size_t cur_n = g_alloc_probed_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < cur_n; ++i) {
        if (g_alloc_probed_dev_vts[i].load(std::memory_order_relaxed) == dvt) return cmd_sig_ok;
    }

    ID3D12CommandAllocator* probe = nullptr;
    if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probe))) && probe) {
        void** vt = *reinterpret_cast<void***>(probe);
        void* fn_ca_release = vt[2]; // IUnknown::Release on CommandAllocator
        void* fn_ca_reset = vt[8];   // ID3D12CommandAllocator::Reset
        auto pred = [](const void* target, std::string* out_mod) {
            return in_hookable_d3d12_module(target, out_mod);
        };
        std::string mod;
        MH_STATUS mh = MH_OK;
        const auto reset_out = openrr::pe::ensure_entry_hook_locked(
            g_ca_reset_hooks, fn_ca_reset, kCaResetDetours, g_known_direct_hook_fns, pred, &mod, &mh);
        if (reset_out == openrr::pe::EntryHookOutcome::RefusedPolicy ||
            reset_out == openrr::pe::EntryHookOutcome::DuplicateOtherHook ||
            reset_out == openrr::pe::EntryHookOutcome::SlotsFull) {
            log("[openrr] allocator Reset slot 8 refused (%s); not hooking", mod.c_str());
        } else if (reset_out == openrr::pe::EntryHookOutcome::CreateFailed ||
                   reset_out == openrr::pe::EntryHookOutcome::EnableFailed) {
            log("[openrr] allocator Reset slot 8 hook failed in %s (status=%d)", mod.c_str(), int(mh));
        }

        openrr::pe::ensure_entry_hook_locked(
            g_ca_release_hooks, fn_ca_release, kCaReleaseDetours, g_known_direct_hook_fns, pred);
        probe->Release();

        if (cur_n < std::size(g_alloc_probed_dev_vts)) {
            g_alloc_probed_dev_vts[cur_n].store(dvt, std::memory_order_relaxed);
            g_alloc_probed_count.store(cur_n + 1, std::memory_order_release);
        }
    }
    return cmd_sig_ok;
}

bool ensure_direct_state_hooks(ID3D12Device* dev, ID3D12GraphicsCommandList* cl = nullptr) {
    const bool cmd_sig_ok = (!dev) ? true : ensure_allocator_reset_hook(dev);
    if (g_direct_hooks_installed.load(std::memory_order_acquire))
        return g_direct_hooks_ok.load(std::memory_order_relaxed) && cmd_sig_ok;
    if (dev && !cmd_sig_ok) {
        static std::atomic<bool> logged_cmd_sig_unavail{false};
        if (!logged_cmd_sig_unavail.exchange(true, std::memory_order_relaxed)) {
            log("[openrr] Direct state hook deferred: CreateCommandSignature (slot 41) unavailable for device");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    if (g_direct_hooks_installed.load(std::memory_order_relaxed))
        return g_direct_hooks_ok.load(std::memory_order_relaxed) && cmd_sig_ok;

    ID3D12CommandAllocator* temp_alloc = nullptr;
    ID3D12GraphicsCommandList* temp_cl = nullptr;
    if (!cl && dev) {
        if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&temp_alloc))) && temp_alloc) {
            if (SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, temp_alloc, nullptr, IID_PPV_ARGS(&temp_cl))) && temp_cl) {
                temp_cl->Close();
                cl = temp_cl;
            }
        }
    }
    struct TempHold {
        ID3D12GraphicsCommandList* l;
        ID3D12CommandAllocator* a;
        ~TempHold() {
            if (l) l->Release();
            if (a) a->Release();
        }
    } temp_hold{temp_cl, temp_alloc};

    if (!cl) return false;

    void** vt = *reinterpret_cast<void***>(cl);
    const openrr::pe::BatchHookItem cl_items[] = {
        {"Reset",                             10, vt[10], reinterpret_cast<void*>(&hk_cl_reset),             reinterpret_cast<void**>(&real_cl_reset)},
        {"ClearState",                        11, vt[11], reinterpret_cast<void*>(&hk_cl_clear_state),       reinterpret_cast<void**>(&real_cl_clear_state)},
        {"SetPipelineState",                  25, vt[25], reinterpret_cast<void*>(&hk_cl_set_pso),           reinterpret_cast<void**>(&real_cl_set_pso)},
        {"ExecuteBundle",                     27, vt[27], reinterpret_cast<void*>(&hk_cl_exec_bundle),       reinterpret_cast<void**>(&real_cl_exec_bundle)},
        {"SetDescriptorHeaps",                28, vt[28], reinterpret_cast<void*>(&hk_cl_set_heaps),          reinterpret_cast<void**>(&real_cl_set_heaps)},
        {"SetComputeRootSignature",           29, vt[29], reinterpret_cast<void*>(&hk_cl_set_compute_rs),     reinterpret_cast<void**>(&real_cl_set_compute_rs)},
        {"SetComputeRootDescriptorTable",     31, vt[31], reinterpret_cast<void*>(&hk_cl_set_compute_table),  reinterpret_cast<void**>(&real_cl_set_compute_table)},
        {"SetComputeRoot32BitConstant",       33, vt[33], reinterpret_cast<void*>(&hk_cl_set_compute_const),  reinterpret_cast<void**>(&real_cl_set_compute_const)},
        {"SetComputeRoot32BitConstants",      35, vt[35], reinterpret_cast<void*>(&hk_cl_set_compute_consts), reinterpret_cast<void**>(&real_cl_set_compute_consts)},
        {"SetComputeRootConstantBufferView",  37, vt[37], reinterpret_cast<void*>(&hk_cl_set_compute_cbv),    reinterpret_cast<void**>(&real_cl_set_compute_cbv)},
        {"SetComputeRootShaderResourceView",  39, vt[39], reinterpret_cast<void*>(&hk_cl_set_compute_srv),    reinterpret_cast<void**>(&real_cl_set_compute_srv)},
        {"SetComputeRootUnorderedAccessView", 41, vt[41], reinterpret_cast<void*>(&hk_cl_set_compute_uav),    reinterpret_cast<void**>(&real_cl_set_compute_uav)},
        {"ExecuteIndirect",                   59, vt[59], reinterpret_cast<void*>(&hk_cl_exec_indirect),      reinterpret_cast<void**>(&real_cl_exec_indirect)},
    };

    std::vector<void*> batch_seen;
    batch_seen.reserve(std::size(cl_items));
    std::string mod_name;
    for (const auto& it : cl_items) {
        if (!in_hookable_d3d12_module(it.target, &mod_name)) {
            log("[openrr] Direct state hook refused: command-list %s (slot %u) is in non-hookable target (%s)",
                it.name, it.slot, mod_name.c_str());
            g_direct_hooks_ok.store(false, std::memory_order_relaxed);
            g_direct_hooks_installed.store(true, std::memory_order_release);
            return false;
        }
        if (std::find(g_known_direct_hook_fns.begin(), g_known_direct_hook_fns.end(), it.target) != g_known_direct_hook_fns.end() ||
            std::find(batch_seen.begin(), batch_seen.end(), it.target) != batch_seen.end()) {
            log("[openrr] Direct state hook refused: duplicate target function on %s (slot %u) in %s",
                it.name, it.slot, mod_name.c_str());
            g_direct_hooks_ok.store(false, std::memory_order_relaxed);
            g_direct_hooks_installed.store(true, std::memory_order_release);
            return false;
        }
        batch_seen.push_back(it.target);
    }

    const auto batch_res = openrr::pe::install_hook_batch(cl_items, std::size(cl_items));
    if (batch_res.error != openrr::pe::BatchInstallError::Ok) {
        if (batch_res.error == openrr::pe::BatchInstallError::ApplyFailed) {
            log("[openrr] Direct state hook batch enable failed (status=%d, trampolines_retained=%d); disabling Direct",
                int(batch_res.mh_status), int(batch_res.trampolines_retained));
        } else {
            const auto& fail_it = cl_items[batch_res.failed_index];
            log("[openrr] Direct state hook failed to %s on %s (slot %u, status=%d)",
                batch_res.error == openrr::pe::BatchInstallError::CreateFailed ? "create" : "queue",
                fail_it.name, fail_it.slot, int(batch_res.mh_status));
        }
        if (batch_res.trampolines_retained) {
            for (void* t : batch_seen) {
                if (std::find(g_known_direct_hook_fns.begin(), g_known_direct_hook_fns.end(), t) == g_known_direct_hook_fns.end())
                    g_known_direct_hook_fns.push_back(t);
            }
        }
        g_direct_hooks_ok.store(false, std::memory_order_relaxed);
        g_direct_hooks_installed.store(true, std::memory_order_release);
        return false;
    }

    for (void* t : batch_seen) g_known_direct_hook_fns.push_back(t);

    std::vector<openrr::pe::BatchHookItem> opt_created;
    opt_created.reserve(6);
    auto create_optional = [&](const char* name, unsigned slot, void* fn, void* detour, void** orig) -> bool {
        if (!in_hookable_d3d12_module(fn)) return false;
        if (std::find(g_known_direct_hook_fns.begin(), g_known_direct_hook_fns.end(), fn) != g_known_direct_hook_fns.end())
            return false;
        for (const auto& existing : opt_created) {
            if (existing.target == fn) return false;
        }
        if (MH_CreateHook(fn, detour, orig) != MH_OK) {
            *orig = nullptr;
            return false;
        }
        opt_created.push_back({name, slot, fn, detour, orig});
        return true;
    };

    create_optional("Release", 2, vt[2], reinterpret_cast<void*>(&hk_cl_release), reinterpret_cast<void**>(&real_cl_release));

    bool diag_enabled = false;
    bool legacy_created = false;
    bool enhanced_created = false;
    if (openrr::pe::d3d12_diagnostics_enabled()) {
        diag_enabled = true;
        legacy_created = create_optional("ResourceBarrier", 26, vt[26], reinterpret_cast<void*>(&hk_cl_barrier),
                                         reinterpret_cast<void**>(&real_cl_barrier));
        ID3D12GraphicsCommandList7* cl7 = nullptr;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl7))) && cl7) {
            void** vt7 = *reinterpret_cast<void***>(cl7);
            enhanced_created = create_optional("Barrier", 80, vt7[80], reinterpret_cast<void*>(&hk_cl_barrier7),
                                               reinterpret_cast<void**>(&real_cl_barrier7));
            cl7->Release();
        }
    }

    IUnknown* cl4 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(kIID_List4, reinterpret_cast<void**>(&cl4))) && cl4) {
        void** vt4 = *reinterpret_cast<void***>(cl4);
        create_optional("SetPipelineState1", 75, vt4[75], reinterpret_cast<void*>(&hk_cl_set_pso1),
                        reinterpret_cast<void**>(&real_cl_set_pso1));
        cl4->Release();
    }

    if (dev) {
        void** dvt = *reinterpret_cast<void***>(dev);
        create_optional("CreateCommandList", 12, dvt[12], reinterpret_cast<void*>(&hk_dev_create_cl),
                        reinterpret_cast<void**>(&real_dev_create_cl));
        IUnknown* dev4 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(kIID_Dev4, reinterpret_cast<void**>(&dev4))) && dev4) {
            void** dvt4 = *reinterpret_cast<void***>(dev4);
            create_optional("CreateCommandList1", 51, dvt4[51], reinterpret_cast<void*>(&hk_dev4_create_cl1),
                            reinterpret_cast<void**>(&real_dev4_create_cl1));
            dev4->Release();
        }
    }

    bool opt_ok = true;
    if (!opt_created.empty()) {
        const auto opt_res = openrr::pe::enable_created_hook_batch(opt_created.data(), opt_created.size());
        opt_ok = (opt_res.error == openrr::pe::BatchInstallError::Ok);
        if (opt_ok || opt_res.trampolines_retained) {
            for (const auto& item : opt_created) g_known_direct_hook_fns.push_back(item.target);
        }
        if (!opt_ok) {
            log("[openrr] Direct optional hook batch enable failed (phase=%d, status=%d, retained=%d)",
                int(opt_res.error), int(opt_res.mh_status), int(opt_res.trampolines_retained));
        }
    }
    if (diag_enabled) {
        log("[d3d12 diag] host barrier hooks legacy=%d enhanced=%d",
            int(legacy_created && opt_ok), int(enhanced_created && opt_ok));
    }

    g_direct_hooks_ok.store(true, std::memory_order_relaxed);
    g_direct_hooks_installed.store(true, std::memory_order_release);
    log("[openrr] Direct compute-state hooks installed in %s (13 list methods + CreateCommandList + Allocator::Reset)",
        mod_name.c_str());
    return true;
}

void log_direct_texture_barriers(ID3D12GraphicsCommandList* cmd, const OnrRrFrame& frame) {
    const auto* state = direct_state_of(cmd);
    const std::pair<const char*, const OnrRrTexture*> inputs[] = {
        {"Color", &frame.color}, {"Output", &frame.output}, {"Depth", &frame.depth}, {"MV", &frame.motion},
        {"DiffuseAlbedo", &frame.diffuse_albedo}, {"SpecularAlbedo", &frame.specular_albedo},
        {"Normals", &frame.normals}, {"Roughness", &frame.roughness},
        {"HitDistance", &frame.specular_hit_distance}, {"DisocclusionMask", &frame.disocclusion_mask},
    };
    for (const auto& [name, tex] : inputs) {
        if (!tex->resource) continue;
        const auto* b = state->last_barrier(tex->resource);
        if (!b) {
            log("[d3d12 diag] %s resource=%p declared=0x%X last_host_barrier=unknown", name, tex->resource, tex->state);
        } else if (b->enhanced) {
            log("[d3d12 diag] %s resource=%p declared=0x%X enhanced layout=%u->%u access=0x%X->0x%X"
                " flags=0x%X range=%u,%u,%u,%u,%u,%u",
                name, tex->resource, tex->state, b->before, b->after, b->access_before, b->access_after, b->flags,
                b->range.IndexOrFirstMipLevel, b->range.NumMipLevels, b->range.FirstArraySlice,
                b->range.NumArraySlices, b->range.FirstPlane, b->range.NumPlanes);
        } else {
            log("[d3d12 diag] %s resource=%p declared=0x%X legacy state=0x%X->0x%X flags=0x%X subresource=%u",
                name, tex->resource, tex->state, b->before, b->after, b->flags, b->range.IndexOrFirstMipLevel);
        }
    }
}

bool restore_direct_state(ID3D12GraphicsCommandList* cmd) {
    const DirectListState* s = direct_state_of(cmd);
    if (!s || !s->valid || s->unknown) return false;

    ++t_in_openrr_record;
    const DirectRootArgs& r = s->compute_root;
    if (r.rs_set && r.rs) {
        cmd->SetComputeRootSignature(r.rs);
        for (UINT i = 0; i < 64; ++i) {
            if (r.mask & (1ull << i)) {
                switch (r.kind[i]) {
                    case 1: cmd->SetComputeRootDescriptorTable(i, {r.value[i]}); break;
                    case 2: cmd->SetComputeRootConstantBufferView(i, r.value[i]); break;
                    case 3: cmd->SetComputeRootShaderResourceView(i, r.value[i]); break;
                    case 4: cmd->SetComputeRootUnorderedAccessView(i, r.value[i]); break;
                    default: break;
                }
            }
            if ((r.const_param_mask & (1ull << i)) && r.const_vals[i]) {
                uint64_t dm = r.const_dword_mask[i];
                while (dm) {
                    const UINT start = UINT(__builtin_ctzll(dm));
                    UINT count = 0;
                    while (start + count < 64 && (dm & (1ull << (start + count)))) ++count;
                    cmd->SetComputeRoot32BitConstants(i, count, &(*r.const_vals[i])[start], start);
                    const uint64_t run = (count == 64) ? ~0ull : ((1ull << count) - 1ull);
                    dm &= ~(run << start);
                }
            }
        }
    }
    if (s->so_last && s->so_set && s->state_object) {
        ID3D12GraphicsCommandList4* cl4 = nullptr;
        if (SUCCEEDED(cmd->QueryInterface(kIID_List4, reinterpret_cast<void**>(&cl4))) && cl4) {
            cl4->SetPipelineState1(s->state_object);
            cl4->Release();
        }
    } else if (s->pso_set && s->pso) {
        cmd->SetPipelineState(s->pso);
    }
    --t_in_openrr_record;
    return true;
}

// ---- Runtime API & nvngx_dlssd.dll discovery ----
std::mutex g_rt_mutex;
OnrRrApi5 g_api5{};
OnrRrApi4& g_api4 = g_api5.v4;
OnrRrApi& g_api = g_api5.v4.v3.v2.v1;
bool g_api_ready = false;

std::vector<std::wstring> g_ngx_search_paths;
std::wstring g_dlssd_path;
std::string g_blocked_reason;

bool ensure_api() {
    if (g_api_ready) return true;
    if (OnrRrGetApi(ONR_RR_ABI_VERSION_5, &g_api) == ONR_RR_OK && g_api.Check && g_api.Create && g_api.Destroy) {
        g_api_ready = true;
        return true;
    }
    return false;
}

std::string runtime_last_error() {
    char buf[256]{};
    if (ensure_api() && g_api.GetLastError) g_api.GetLastError(buf, sizeof buf);
    return buf;
}

void note_feature_common_info(const NVSDK_NGX_FeatureCommonInfo* info) {
    if (!info || !info->PathListInfo.Path || info->PathListInfo.Length == 0 || info->PathListInfo.Length > 64) return;
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    for (unsigned int i = 0; i < info->PathListInfo.Length; ++i) {
        const wchar_t* p = info->PathListInfo.Path[i];
        if (!p || !p[0]) continue;
        std::wstring ws(p);
        if (std::find(g_ngx_search_paths.begin(), g_ngx_search_paths.end(), ws) == g_ngx_search_paths.end())
            g_ngx_search_paths.push_back(std::move(ws));
    }
}

std::wstring find_dlssd_locked(const char** out_source) {
    std::error_code ec;
    const std::filesystem::path opti_dir = wide_from_utf8(openrr::pe::module_folder());

    const auto data_dir = openrr::pe::installed_data_dir(opti_dir, L"nvngx_dlssd.dll");
    if (!data_dir.empty()) {
        if (out_source) *out_source = "installed OpenRR data folder";
        return (data_dir / L"nvngx_dlssd.dll").wstring();
    }
    const auto beside_host = opti_dir / L"nvngx_dlssd.dll";
    if (std::filesystem::exists(beside_host, ec)) {
        if (out_source) *out_source = "OptiScaler folder";
        return beside_host.wstring();
    }

    if (HMODULE loaded = GetModuleHandleW(L"nvngx_dlssd.dll")) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(loaded, path, MAX_PATH) != 0) {
            if (out_source) *out_source = "loaded in process";
            return path;
        }
    }

    for (const auto& dir : g_ngx_search_paths) {
        const auto cand = std::filesystem::path(dir) / L"nvngx_dlssd.dll";
        if (std::filesystem::exists(cand, ec)) {
            if (out_source) *out_source = "NGX PathListInfo";
            return cand.wstring();
        }
    }

    wchar_t exe_buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) != 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        const auto beside_exe = exe_dir / L"nvngx_dlssd.dll";
        if (std::filesystem::exists(beside_exe, ec)) {
            if (out_source) *out_source = "beside game exe";
            return beside_exe.wstring();
        }
        // Walk up to 2 parent levels and 3 child levels (e.g. Cyberpunk bin\x64 or Streamline subfolders)
        std::filesystem::path root = exe_dir;
        for (int up = 0; up < 2; ++up) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (it.depth() > 3) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), L"nvngx_dlssd.dll") == 0) {
                    if (out_source) *out_source = "game directory tree";
                    return it->path().wstring();
                }
            }
            if (root.has_parent_path()) root = root.parent_path();
        }
    }

    if (out_source) *out_source = "not found";
    return {};
}

std::wstring resolve_data_dir() {
    std::error_code ec;
    const std::filesystem::path opti_dir = wide_from_utf8(openrr::pe::module_folder());
    const wchar_t legacy_name[] = {L'o', L'p', L'e', L'n', L'n', L'r', L'-', L'r', L'r', 0};
    if (std::filesystem::exists(opti_dir / legacy_name, ec)) {
        static std::atomic<bool> logged_legacy{false};
        if (!logged_legacy.exchange(true, std::memory_order_relaxed)) {
            log("[openrr] note: legacy %ls\\ directory detected and ignored; using openrr-amd\\", legacy_name);
        }
    }
    const auto data_dir = openrr::pe::installed_data_dir(opti_dir, L"shaders");
    if (!data_dir.empty()) return data_dir.wstring();

    wchar_t self_buf[MAX_PATH]{};
    if (g_self && GetModuleFileNameW(g_self, self_buf, MAX_PATH) != 0) {
        const std::filesystem::path asi_dir = std::filesystem::path(self_buf).parent_path();
        if (std::filesystem::exists(asi_dir / L"openrr-amd" / L"shaders", ec))
            return (asi_dir / L"openrr-amd").wstring();
        if (std::filesystem::exists(asi_dir / L"shaders", ec))
            return asi_dir.wstring();
    }
    return (opti_dir / L"openrr-amd").wstring();
}

std::wstring resolve_log_path() {
    return wide_from_utf8(openrr::pe::log_path());
}

void block_session(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    if (g_blocked_reason.empty()) {
        g_blocked_reason = why;
        log("[openrr] session blocked: %s", why.c_str());
    }
}

uint64_t file_mtime_64(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

bool check_dlssd_version_310_9_1(const std::wstring& path, std::string& why) {
    static HMODULE ver_mod = LoadLibraryW(L"version.dll");
    using PFN_GetSize = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using PFN_GetInfo = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using PFN_Query   = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    static auto fn_size  = ver_mod ? reinterpret_cast<PFN_GetSize>(GetProcAddress(ver_mod, "GetFileVersionInfoSizeW")) : nullptr;
    static auto fn_info  = ver_mod ? reinterpret_cast<PFN_GetInfo>(GetProcAddress(ver_mod, "GetFileVersionInfoW")) : nullptr;
    static auto fn_query = ver_mod ? reinterpret_cast<PFN_Query>(GetProcAddress(ver_mod, "VerQueryValueW")) : nullptr;
    if (!fn_size || !fn_info || !fn_query) {
        why = "version.dll entry points unavailable";
        return false;
    }
    DWORD dummy = 0;
    const DWORD sz = fn_size(path.c_str(), &dummy);
    if (!sz) {
        why = "nvngx_dlssd.dll has no version info resource";
        return false;
    }
    std::vector<uint8_t> buf(sz);
    if (!fn_info(path.c_str(), 0, sz, buf.data())) {
        why = "GetFileVersionInfoW failed on nvngx_dlssd.dll";
        return false;
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!fn_query(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi || len < sizeof(VS_FIXEDFILEINFO)) {
        why = "VerQueryValueW failed on nvngx_dlssd.dll";
        return false;
    }
    const unsigned v0 = HIWORD(ffi->dwFileVersionMS);
    const unsigned v1 = LOWORD(ffi->dwFileVersionMS);
    const unsigned v2 = HIWORD(ffi->dwFileVersionLS);
    if (v0 != 310 || v1 != 9 || v2 != 1) {
        char b[128];
        std::snprintf(b, sizeof b, "nvngx_dlssd.dll is version %u.%u.%u (310.9.1 required)", v0, v1, v2);
        why = b;
        return false;
    }
    return true;
}

} // namespace

namespace {

bool check_gpu_cheap_vulkan(const LUID* luid, std::string& out_gpu_name, std::string& why) {
    InternalVkScope vk_scope;
    openrr::pe::vulkan_entry_points();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    const VkResult ir = vkCreateInstance(&ici, nullptr, &inst);
    if (ir != VK_SUCCESS || !inst) {
        why = "vkCreateInstance failed (" + std::to_string(int(ir)) + ")";
        return false;
    }
    struct InstHold { VkInstance i; ~InstHold() { if (i) vkDestroyInstance(i, nullptr); } } hold{inst};

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(inst, &count, nullptr) != VK_SUCCESS || !count) {
        why = "no Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(inst, &count, devs.data());

    std::string picked_name;
    uint32_t picked_api_version = 0;
    bool picked_is_amd = false;
    const VkPhysicalDevice picked = onr::pick_vk_physical_device(
        devs, reinterpret_cast<const uint8_t*>(luid), &picked_name, &picked_api_version, &picked_is_amd);
    if (!picked) {
        why = luid ? "adapter LUID not found in Vulkan" : "no discrete AMD GPU found in Vulkan";
        return false;
    }
    const onr::TrustedVkIdentity tid = onr::identify_vk_physical_device(picked);
    out_gpu_name = tid.name.empty() ? picked_name : tid.name;
    const bool trusted_amd = (tid.vendor == 0x1002) || (tid.arch != onr::arch::GpuArch::Unknown) || picked_is_amd;
    if (tid.arch == onr::arch::GpuArch::Rdna1 || tid.arch == onr::arch::GpuArch::Rdna2) {
        why = out_gpu_name + ": " + onr::arch::arch_name(tid.arch) + " GPU is not supported (requires RDNA3 or RDNA4)";
        return false;
    }

    if (picked_api_version < VK_API_VERSION_1_3) {
        why = out_gpu_name + ": Vulkan API older than 1.3";
        return false;
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(picked, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(picked, nullptr, &ne, exts.data());
    auto has_ext = [&](const char* name) {
        return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& x) {
            return std::strcmp(x.extensionName, name) == 0;
        });
    };
    const bool has_cm = has_ext(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    if (!has_cm) {
        why = out_gpu_name + (trusted_amd ? ": no cooperative matrices (needs an RDNA4 or RDNA3 GPU)" : ": not an AMD GPU");
        return false;
    }
    const bool has_f8 = has_ext(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);

    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
                                                    has_f8 ? &f8 : nullptr};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &cm};
    vkGetPhysicalDeviceFeatures2(picked, &f2);
    if (!cm.cooperativeMatrix) {
        why = out_gpu_name + ": no cooperative matrices (needs an RDNA4 or RDNA3 GPU)";
        return false;
    }
    const bool fp8_coopmat = has_f8 && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
    if (!fp8_coopmat) {
        if (!trusted_amd || (tid.arch != onr::arch::GpuArch::Unknown && !onr::arch::has_runtime_support(tid.arch))) {
            why = out_gpu_name + ": not a supported AMD RDNA3/RDNA4 GPU";
            return false;
        }
        std::error_code ec;
        const std::filesystem::path gfx11_dir = std::filesystem::path(resolve_data_dir()) / L"shaders-gfx11";
        if (!std::filesystem::is_directory(gfx11_dir, ec)) {
            why = out_gpu_name + ": not an RDNA4 GPU, and no RDNA3 kernels (openrr-amd\\shaders-gfx11)";
            return false;
        }
    }
    return true;
}

struct UsabilityCheck {
    bool usable = false;
    ULONGLONG tick = 0;
    uint64_t mtime = 0;
    std::wstring path;
};
// One cached result per adapter LUID (plus the automatic selection): an iGPU that fails the check
// (e.g. enumerated by Streamline) must not disable OpenRR for the dGPU, and vice versa.
std::map<std::pair<bool, uint64_t>, UsabilityCheck> g_usability_checks;

bool check_usable(const LUID* luid) {
    if (!openrr::pe::current_config().enabled) return false;
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    if (!g_blocked_reason.empty()) return false;
    if (!ensure_api()) return false;

    uint64_t id = 0;
    if (luid) std::memcpy(&id, luid, sizeof(id));
    auto& cached = g_usability_checks[{luid != nullptr, id}];
    const ULONGLONG now = GetTickCount64();
    if (cached.tick && ((!cached.usable && now - cached.tick < 30000) ||
        (cached.usable && cached.mtime && file_mtime_64(cached.path) == cached.mtime))) {
        g_dlssd_path = cached.path;
        return cached.usable;
    }
    bool usable = false;
    struct SaveCheck {
        UsabilityCheck& entry;
        ULONGLONG now;
        bool& usable;
        ~SaveCheck() {
            entry = {usable, now, g_dlssd_path.empty() ? 0 : file_mtime_64(g_dlssd_path), g_dlssd_path};
        }
    } save{cached, now, usable};

    const auto t0 = std::chrono::steady_clock::now();
    const char* source = nullptr;
    g_dlssd_path = find_dlssd_locked(&source);

    if (g_dlssd_path.empty()) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: nvngx_dlssd.dll (310.9.1) not found", ms);
        return false;
    }

    std::string why;
    if (!check_dlssd_version_310_9_1(g_dlssd_path, why)) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: %s (%s)", ms, why.c_str(), utf8_from_wide(g_dlssd_path).c_str());
        return false;
    }

    std::string gpu_name;
    if (!check_gpu_cheap_vulkan(luid, gpu_name, why)) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: %s", ms, why.c_str());
        return false;
    }

    usable = true;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log("[openrr] Check passed (cheap check in %.1f ms; GPU: %s; dlssd from %s: %s)",
        ms, gpu_name.c_str(), source ? source : "", utf8_from_wide(g_dlssd_path).c_str());
    return true;
}

// ---- Engine & Barrier config from OptiScaler.ini ----
bool g_unreal_by_exe = false;
bool g_game_colour_readable = false;
bool g_game_motion_readable = false;
int g_barrier_color = -1, g_barrier_mv = -1, g_barrier_depth = -1, g_barrier_output = -1;

std::wstring lower_w(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}
bool contains_any(const std::wstring& s, std::initializer_list<const wchar_t*> words) {
    for (const wchar_t* w : words)
        if (s.find(w) != std::wstring::npos) return true;
    return false;
}

// OptiScaler's Util.cpp game-engine check, mirrored verbatim from dlssnr-amd (Unity first, then Unreal).
void judge_executable() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    const std::wstring full = path;
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring exe = lower_w(slash == std::wstring::npos ? full : full.substr(slash + 1));
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
    auto version_string = [&](const wchar_t* name) -> std::wstring {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &handle);
        if (!size) return {};
        std::vector<uint8_t> data(size);
        if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};
        struct Translation { WORD language, codepage; }* tr = nullptr;
        UINT n = 0;
        if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &n) || !tr ||
            n < sizeof(Translation))
            return {};
        wchar_t query[128];
        std::swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr->language, tr->codepage, name);
        LPWSTR value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID*>(&value), &len) && value && len) return value;
        return {};
    };
    const std::wstring product = lower_w(version_string(L"ProductName")),
                       copyright = lower_w(version_string(L"LegalCopyright")),
                       company = lower_w(version_string(L"CompanyName")),
                       description = lower_w(version_string(L"FileDescription"));
    const bool unity = GetFileAttributesW((dir + L"\\UnityPlayer.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
                       GetFileAttributesW((dir + L"\\GameAssembly.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
                       contains_any(product, {L"unity"}) || contains_any(company, {L"unity"}) ||
                       contains_any(copyright, {L"unity"}) || contains_any(description, {L"unity"});
    if (unity) return;
    auto ends = [&](const wchar_t* tail) {
        const size_t t = std::wcslen(tail);
        return exe.size() >= t && exe.compare(exe.size() - t, t, tail) == 0;
    };
    const bool shipping = ends(L"-win64-shipping.exe") || ends(L"-wingdk-shipping.exe") || ends(L"-windows-shipping.exe") ||
                          ends(L"-win64-test.exe") || ends(L"-win64-debug.exe") || ends(L"-windows-test.exe") ||
                          ends(L"-windows-debug.exe");
    bool binaries = false;
    {
        const size_t a = dir.find_last_of(L"\\/");
        if (a != std::wstring::npos) {
            const std::wstring platform = lower_w(dir.substr(a + 1)), parent = dir.substr(0, a);
            const size_t b = parent.find_last_of(L"\\/");
            const std::wstring bins = lower_w(b == std::wstring::npos ? parent : parent.substr(b + 1));
            binaries = bins == L"binaries" && platform.rfind(L"win", 0) == 0;
        }
    }
    g_unreal_by_exe = contains_any(product, {L"unreal"}) || contains_any(company, {L"epic games"}) ||
                      contains_any(copyright, {L"epic games", L"unreal"}) || contains_any(description, {L"unreal"}) ||
                      shipping || binaries;
    // misc/GameConfigs.cpp: the Unreal games whose colour (and motion vectors) OptiScaler keeps in
    // NON_PIXEL_SHADER_RESOURCE, and the games it takes for Unreal whatever their executable says.
    const std::string name(exe.begin(), exe.end());
    auto ue = [&](const char* project) {
        return name == std::string(project) + "-win64-shipping.exe" || name == std::string(project) + "-wingdk-shipping.exe";
    };
    for (const char* p : {"returnal", "hellbladegame", "outriders", "medium", "sackboy", "pumpkinjack", "dungeonhaven", "pal7"})
        if (ue(p)) g_game_colour_readable = g_game_motion_readable = true;
    if (ue("avowed")) g_game_colour_readable = true;
    if (name == "observersystemredux.exe") g_game_colour_readable = g_game_motion_readable = true;
    if (name == "gwt.exe" || name == "ff7rebirth_.exe" || name == "soulstice.exe") g_unreal_by_exe = true;
}

// [Hotfix] *ResourceBarrier in OptiScaler.ini beside OptiScaler (auto / absent: not set).
void read_ini_barriers() {
    const std::string base = openrr::pe::module_folder();
    const std::string ini = (base.empty() ? std::string() : base + "\\") + "OptiScaler.ini";
    auto get = [&](const char* key) {
        char v[32] = {};
        openrr::u8::profile("Hotfix", key, "", v, sizeof v, ini);
        return v[0] >= '0' && v[0] <= '9' ? std::atoi(v) : -1;
    };
    g_barrier_color = get("ColorResourceBarrier");
    g_barrier_depth = get("DepthResourceBarrier");
    g_barrier_mv = get("MotionVectorResourceBarrier");
    if (g_barrier_mv < 0) g_barrier_mv = get("MVResourceBarrier");
    g_barrier_output = get("OutputResourceBarrier");
}

bool is_unreal_engine() {
    return g_unreal_by_exe;
}

// ---- Optimal settings & stats callbacks for DLSS-D ----
NVSDK_NGX_Result RR_NGX_MSABI hk_dlssd_optimal_settings(NVSDK_NGX_Parameter* InParams) {
    if (!InParams) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    unsigned int Width = 0, Height = 0;
    int PerfQualityValue = 0;
    if (InParams->Get(NVSDK_NGX_Parameter_Width, &Width) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_Height, &Height) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_PerfQualityValue, &PerfQualityValue) != NVSDK_NGX_Result_Success ||
        Width == 0 || Height == 0) {
        return NVSDK_NGX_Result_Fail;
    }

    float divisor = 1.7f;
    switch (static_cast<NVSDK_NGX_PerfQuality_Value>(PerfQualityValue)) {
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance: divisor = 3.0f; break;
        case NVSDK_NGX_PerfQuality_Value_MaxPerf:          divisor = 2.0f; break;
        case NVSDK_NGX_PerfQuality_Value_Balanced:         divisor = 1.7f; break;
        case NVSDK_NGX_PerfQuality_Value_MaxQuality:       divisor = 1.5f; break;
        case NVSDK_NGX_PerfQuality_Value_UltraQuality:     divisor = 1.3f; break;
        case NVSDK_NGX_PerfQuality_Value_DLAA:             divisor = 1.0f; break;
        default:                                           divisor = 1.7f; break;
    }

    const unsigned int OutWidth = std::max(1u, static_cast<unsigned int>(float(Width) / divisor));
    const unsigned int OutHeight = std::max(1u, static_cast<unsigned int>(float(Height) / divisor));
    const float scale = 1.0f / divisor;

    InParams->Set(NVSDK_NGX_Parameter_Scale, scale);
    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, scale);
    InParams->Set(NVSDK_NGX_Parameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_Parameter_OutHeight, OutHeight);

    const bool dlaa = static_cast<NVSDK_NGX_PerfQuality_Value>(PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_DLAA;
    const unsigned int minW = dlaa ? Width : std::min(OutWidth, static_cast<unsigned int>(float(Width) * 0.5f));
    const unsigned int minH = dlaa ? Height : std::min(OutHeight, static_cast<unsigned int>(float(Height) * 0.5f));

    InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, minW);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, minH);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, Width);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, Height);
    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, Width * Height * 31u);
    InParams->Set(NVSDK_NGX_Parameter_DLSSMode, static_cast<int>(NVSDK_NGX_DLSS_Mode_DLSS_DLISP));

    InParams->Set(NVSDK_NGX_EParameter_Scale, scale);
    InParams->Set(NVSDK_NGX_EParameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_EParameter_OutHeight, OutHeight);
    InParams->Set(NVSDK_NGX_EParameter_SizeInBytes, Width * Height * 31u);
    InParams->Set(NVSDK_NGX_EParameter_DLSSMode, static_cast<int>(NVSDK_NGX_DLSS_Mode_DLSS_DLISP));
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_dlssd_get_stats(NVSDK_NGX_Parameter* InParams) {
    if (!InParams) return NVSDK_NGX_Result_Success;
    unsigned int Width = 1920, Height = 1080;
    InParams->Get(NVSDK_NGX_Parameter_Width, &Width);
    InParams->Get(NVSDK_NGX_Parameter_Height, &Height);
    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, Width * Height * 31u);
    return NVSDK_NGX_Result_Success;
}

// ---- Capability checks & parameter population for OptiProvider_Dlssd ----
std::atomic<uint64_t> g_last_d3d12_luid_bits{0};
std::atomic<bool> g_has_d3d12_luid{false};
std::unordered_map<unsigned int, std::shared_ptr<openrr::pe::vkd3d::InteropSession>> g_vkd3d_sessions;

bool d3d12_rr_capable() {
    LUID luid{};
    const LUID* p_luid = nullptr;
    if (g_has_d3d12_luid.load(std::memory_order_acquire)) {
        const uint64_t bits = g_last_d3d12_luid_bits.load(std::memory_order_acquire);
        static_assert(sizeof(LUID) == sizeof(uint64_t));
        std::memcpy(&luid, &bits, sizeof(luid));
        p_luid = &luid;
    }
    return check_usable(p_luid);
}

bool vk_rr_capable(VkPhysicalDevice phys = VK_NULL_HANDLE, VkDevice dev = VK_NULL_HANDLE,
                   bool allow_pre_device = false) {
    openrr::pe::vkdevice::DeviceInfo di{};
    const bool have_dev = dev ? openrr::pe::vkdevice::get_device_info(dev, &di)
                              : openrr::pe::vkdevice::latest_device_info(&di);
    if (have_dev) {
        if (!di.prepared) {
            static std::atomic<bool> logged_unprepared{false};
            if (!logged_unprepared.exchange(true, std::memory_order_relaxed)) {
                log("[openrr] Vulkan capability check: game's VkDevice was not prepared at vkCreateDevice; Ray Reconstruction not offered");
            }
            return false;
        }
        if (!phys) phys = di.physical;
    } else if (!allow_pre_device || !phys) {
        return false;
    }
    if (phys) {
        openrr::pe::vulkan_entry_points();
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(phys, &props);
        if (id.deviceLUIDValid) {
            LUID luid{};
            std::memcpy(&luid, id.deviceLUID, sizeof(luid));
            return check_usable(&luid);
        }
    }
    return check_usable(nullptr);
}

void populate_dlssd_callbacks(NVSDK_NGX_Parameter* params) {
    if (!params) return;
    void* cb = nullptr;
    if (params->Get(NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &cb) != NVSDK_NGX_Result_Success || !cb) {
        params->Set(NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback,
                    reinterpret_cast<void*>(&hk_dlssd_optimal_settings));
    }
    cb = nullptr;
    if (params->Get(NVSDK_NGX_Parameter_DLSSDGetStatsCallback, &cb) != NVSDK_NGX_Result_Success || !cb) {
        params->Set(NVSDK_NGX_Parameter_DLSSDGetStatsCallback,
                    reinterpret_cast<void*>(&hk_dlssd_get_stats));
    }
}

// ---- Parameter reading helpers (HANDOFF §3.4: DLSSD.* first, then generic fallback) ----
ID3D12Resource* get_d3d12_res(const NVSDK_NGX_Parameter& p, const char* k1, const char* k2 = nullptr) {
    auto try_key = [&](const char* k) -> ID3D12Resource* {
        if (!k) return nullptr;
        ID3D12Resource* r = nullptr;
        if (p.Get(k, &r) == NVSDK_NGX_Result_Success && r) return r;
        void* vp = nullptr;
        if (p.Get(k, &vp) == NVSDK_NGX_Result_Success && vp) return static_cast<ID3D12Resource*>(vp);
        return nullptr;
    };
    if (ID3D12Resource* r = try_key(k1)) return r;
    return try_key(k2);
}

NVSDK_NGX_Resource_VK* get_vk_res(const NVSDK_NGX_Parameter& p, const char* k1, const char* k2 = nullptr) {
    auto try_key = [&](const char* k) -> NVSDK_NGX_Resource_VK* {
        if (!k) return nullptr;
        void* vp = nullptr;
        if (p.Get(k, &vp) == NVSDK_NGX_Result_Success && vp) return static_cast<NVSDK_NGX_Resource_VK*>(vp);
        return nullptr;
    };
    if (NVSDK_NGX_Resource_VK* r = try_key(k1)) return r;
    return try_key(k2);
}

// Read create-time dimensions matching OptiScaler's IFeature::SetInitParameters:
// GetOptimalSettingsCallback writes OutWidth/OutHeight = render size into a parameter block that still holds
// Width/Height = display size, and some games pass that block straight to CreateFeature without swapping.
void read_create_sizes(const NVSDK_NGX_Parameter* p, uint32_t& rw, uint32_t& rh, uint32_t& ow, uint32_t& oh) {
    rw = rh = ow = oh = 0;
    if (!p) return;
    unsigned int w = 0, h = 0, out_w = 0, out_h = 0;
    p->Get(NVSDK_NGX_Parameter_Width, &w);
    p->Get(NVSDK_NGX_Parameter_Height, &h);
    // Render.Subrect.Dimensions belongs to Evaluate and can still describe the previous feature after a resize.
    p->Get(NVSDK_NGX_Parameter_OutWidth, &out_w);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &out_h);

    if (w > 16384 || w < 20) w = 0;
    if (h > 16384 || h < 20) h = 0;
    if (out_w > 16384 || out_w < 20) out_w = 0;
    if (out_h > 16384 || out_h < 20) out_h = 0;

    if (w && h && out_w && out_h) {
        ow = std::max(w, out_w);
        oh = std::max(h, out_h);
        rw = std::min(w, out_w);
        rh = std::min(h, out_h);
    } else {
        rw = w ? w : out_w;
        rh = h ? h : out_h;
        ow = out_w ? out_w : rw;
        oh = out_h ? out_h : rh;
    }
}

struct RrCreateParams {
    uint32_t max_render_w = 0, max_render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    uint32_t flags = 0;
    int hw_depth = -1;
    int roughness_packed = -1;
};

RrCreateParams read_create_params(const NVSDK_NGX_Parameter& params) {
    // Streamline can reuse the parameter table for settings queries while RR is initializing.
    RrCreateParams create;
    read_create_sizes(&params, create.max_render_w, create.max_render_h, create.output_w, create.output_h);
    int value = 0;
    if (params.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &value) == NVSDK_NGX_Result_Success)
        create.flags = uint32_t(value);
    if (params.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
        create.hw_depth = value == NVSDK_NGX_DLSS_Depth_Type_HW;
    if (params.Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
        create.roughness_packed = value == NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    return create;
}

// Read per-frame render resolution matching OptiScaler's IFeature::GetRenderResolution.
void read_eval_render_size(const NVSDK_NGX_Parameter* p, uint32_t max_rw, uint32_t max_rh, uint32_t& rw, uint32_t& rh) {
    rw = max_rw;
    rh = max_rh;
    if (!p) return;
    unsigned int sub_w = 0, sub_h = 0;
    if (p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &sub_w) == NVSDK_NGX_Result_Success &&
        p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &sub_h) == NVSDK_NGX_Result_Success &&
        sub_w > 0 && sub_h > 0) {
        rw = std::min(sub_w, max_rw);
        rh = std::min(sub_h, max_rh);
        return;
    }
    unsigned int w = 0, h = 0, out_w = 0, out_h = 0;
    if (p->Get(NVSDK_NGX_Parameter_Width, &w) == NVSDK_NGX_Result_Success &&
        p->Get(NVSDK_NGX_Parameter_Height, &h) == NVSDK_NGX_Result_Success && w > 0 && h > 0) {
        if (p->Get(NVSDK_NGX_Parameter_OutWidth, &out_w) == NVSDK_NGX_Result_Success &&
            p->Get(NVSDK_NGX_Parameter_OutHeight, &out_h) == NVSDK_NGX_Result_Success && out_w > 0 && out_h > 0) {
            const unsigned int cand_w = std::min(w, out_w);
            const unsigned int cand_h = std::min(h, out_h);
            rw = std::min(cand_w, max_rw);
            rh = std::min(cand_h, max_rh);
        } else if (w <= max_rw && h <= max_rh) {
            rw = w;
            rh = h;
        }
    }
}

enum class DepthKind { Unknown, Hardware, GameWritten };

DepthKind classify_d3d12_depth(ID3D12Resource* depth) {
    if (!depth) return DepthKind::Unknown;
    const D3D12_RESOURCE_DESC desc = depth->GetDesc();
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) return DepthKind::Hardware;
    switch (desc.Format) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return DepthKind::Hardware;
        default: break;
    }
    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0)
        return DepthKind::GameWritten;
    return DepthKind::Unknown;
}

bool is_vk_depth_format(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
           format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT;
}

// ---- RR Feature Contexts (D3D12 & Vulkan) ----
constexpr unsigned int kRrHandleBase = 0x4F525200u;
std::atomic<unsigned int> g_next_rr_handle{kRrHandleBase};

struct RuntimeHolder {
    std::mutex mutex;
    void* ctx = nullptr;
};

struct RrContext {
    unsigned int handle_id = 0;
    NVSDK_NGX_Handle* allocated_handle = nullptr;
    bool is_vk = false;
    ID3D12Device* d3d12_device = nullptr;
    VkDevice vk_device = VK_NULL_HANDLE;
    void* runtime_ctx = nullptr;
    std::shared_ptr<RuntimeHolder> holder = std::make_shared<RuntimeHolder>();
    uint32_t max_render_w = 0, max_render_h = 0;
    uint32_t output_w = 0, output_h = 0;
    uint32_t create_flags = 0;
    bool has_ngx_depth_type = false;
    bool ngx_hw_depth = true;
    int roughness_packed = -1;
    int mode_applied = -1;
    uint64_t frame_count = 0;
    uint32_t unknown_streak = 0;
    ULONGLONG last_eval_tick = 0;
    bool guides_logged = false;
    int logged_hw_depth = -1;
    uint32_t ngx_log_count = 0;
    uint64_t ngx_log_sig = ~0ull;
    bool ready_logged = false;
    OnrRrVkImage vk_images[24]{};
    int num_vk_images = 0;
};

std::mutex g_ctx_mutex;
std::unordered_map<unsigned int, std::unique_ptr<RrContext>> g_d3d12_rr;
std::unordered_map<unsigned int, std::unique_ptr<RrContext>> g_vk_rr;
std::atomic<int> g_ngx_hw_depth{-1};
std::atomic<int> g_ngx_roughness_packed{-1};

void apply_create_params(RrContext& ctx, const RrCreateParams& create) {
    ctx.max_render_w = create.max_render_w;
    ctx.max_render_h = create.max_render_h;
    ctx.output_w = create.output_w;
    ctx.output_h = create.output_h;
    ctx.create_flags = create.flags;
    if (create.hw_depth >= 0) g_ngx_hw_depth.store(create.hw_depth);
    const int hw_depth = create.hw_depth >= 0 ? create.hw_depth : g_ngx_hw_depth.load();
    ctx.has_ngx_depth_type = hw_depth >= 0;
    ctx.ngx_hw_depth = hw_depth != 0;
    if (create.roughness_packed >= 0) g_ngx_roughness_packed.store(create.roughness_packed);
    ctx.roughness_packed = create.roughness_packed >= 0 ? create.roughness_packed : g_ngx_roughness_packed.load();
}

void log_d3d12_ngx_inputs(RrContext& ctx, const NVSDK_NGX_Parameter& params, const OnrRrFrame& frame) {
    if (ctx.ngx_log_count >= 16) return;

    struct KeyInfo {
        const char* name;
        bool ignored;
    };
    static constexpr KeyInfo kKeys[] = {
        {NVSDK_NGX_Parameter_Color, false},
        {NVSDK_NGX_Parameter_Output, false},
        {NVSDK_NGX_Parameter_Depth, false},
        {NVSDK_NGX_Parameter_MotionVectors, false},
        {NVSDK_NGX_Parameter_DiffuseAlbedo, false},
        {NVSDK_NGX_Parameter_SpecularAlbedo, false},
        {NVSDK_NGX_Parameter_GBuffer_Normals, false},
        {NVSDK_NGX_Parameter_GBuffer_Roughness, false},
        {NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, false},
        {NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance, false},
        {NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, false},
        {"DLSS.TransparencyLayer", false},
        {"DLSS.TransparencyLayerOpacity", false},
        {"DLSS.TransparencyLayerMvecs", false},
        {"DLSS.DisocclusionMask", false},
        {NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, false},
        {NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, false},
        {"DLSSD.ResponsivityMask", false},
        {NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, false},
        {NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, false},
        {NVSDK_NGX_Parameter_GBuffer_SpecularMvec, false},
        {"RayTracingHitDistance", true},
        {"MotionVectorsReflection", true},
    };

    uint64_t sig = 14695981039346656037ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            sig ^= (v >> (i * 8)) & 0xFFu;
            sig *= 1099511628211ull;
        }
    };

    std::string detail;
    int present_count = 0;
    for (size_t i = 0; i < std::size(kKeys); ++i) {
        ID3D12Resource* res = get_d3d12_res(params, kKeys[i].name, nullptr);
        if (!res) continue;
        ++present_count;
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        mix(uint64_t(i + 1));
        mix(uint64_t(d.Width));
        mix(uint64_t(d.Height));
        mix(uint64_t(d.Format));
        mix(uint64_t(d.Flags));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s=%p(%llux%u fmt=%d flags=0x%X)%s",
                      detail.empty() ? "" : ", ",
                      kKeys[i].name,
                      static_cast<void*>(res),
                      static_cast<unsigned long long>(d.Width),
                      static_cast<unsigned int>(d.Height),
                      static_cast<int>(d.Format),
                      static_cast<unsigned int>(d.Flags),
                      kKeys[i].ignored ? "[ignored]" : "");
        detail += buf;
    }

    if (sig == ctx.ngx_log_sig) return;
    ctx.ngx_log_sig = sig;
    ++ctx.ngx_log_count;
    log("[openrr] NGX D3D12 inputs (#%u, count=%d, render=%ux%u, jitter=(%.4f,%.4f), mv_scale=(%.4f,%.4f), sub_col=(%u,%u), sub_out=(%u,%u), flags=0x%X): %s",
        ctx.ngx_log_count, present_count,
        frame.render_width, frame.render_height,
        frame.jitter_x, frame.jitter_y,
        frame.mv_scale_x, frame.mv_scale_y,
        frame.color.x, frame.color.y,
        frame.output.x, frame.output.y,
        ctx.create_flags,
        detail.c_str());
    openrr::pe::flush_log();
}

bool fill_d3d12_frame(RrContext& ctx, const NVSDK_NGX_Parameter& params, OnrRrFrame& frame) {
    const openrr::pe::Config cfg = openrr::pe::current_config();
    frame = {};
    frame.size = sizeof(frame);

    read_eval_render_size(&params, ctx.max_render_w, ctx.max_render_h, frame.render_width, frame.render_height);

    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &frame.jitter_x);
    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &frame.jitter_y);
    if (!std::isfinite(frame.jitter_x)) frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y)) frame.jitter_y = 0.0f;

    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.mv_scale_x) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.mv_scale_y) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_y) || frame.mv_scale_y == 0.0f)
        frame.mv_scale_y = 1.0f;

    const bool depth_inverted = (ctx.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    openrr::camera::ResolveCameraMatrices(params, depth_inverted, frame.world_to_view, frame.view_to_clip);

    const bool unreal = is_unreal_engine();
    const uint32_t read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const uint32_t color_state = g_barrier_color >= 0
                                     ? uint32_t(g_barrier_color)
                                     : (g_game_colour_readable ? read_state : (unreal ? D3D12_RESOURCE_STATE_RENDER_TARGET : read_state));
    const uint32_t mv_state = g_barrier_mv >= 0
                                  ? uint32_t(g_barrier_mv)
                                  : (g_game_motion_readable ? read_state : (unreal ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : read_state));
    const uint32_t depth_state = g_barrier_depth >= 0 ? uint32_t(g_barrier_depth) : read_state;
    const uint32_t output_state = g_barrier_output >= 0 ? uint32_t(g_barrier_output) : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    auto tex = [&](const char* key, const char* bx, const char* by, uint32_t st) {
        OnrRrTexture t{};
        if (ID3D12Resource* r = get_d3d12_res(params, key, nullptr)) {
            t.resource = r;
            t.state = st;
            if (bx) params.Get(bx, &t.x);
            if (by) params.Get(by, &t.y);
        }
        return t;
    };

    // Exact NGX parameter keys & subrects matching OpenNrRrFeature_Dx12.cpp (nvngx_dlssd.dll 310.9.1).
    frame.color = tex(NVSDK_NGX_Parameter_Color,
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, color_state);
    frame.diffuse_albedo = tex(NVSDK_NGX_Parameter_DiffuseAlbedo,
                               NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                               NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read_state);
    frame.specular_albedo = tex(NVSDK_NGX_Parameter_SpecularAlbedo,
                                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read_state);
    frame.normals = tex(NVSDK_NGX_Parameter_GBuffer_Normals,
                        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read_state);
    frame.roughness = tex(NVSDK_NGX_Parameter_GBuffer_Roughness,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read_state);
    frame.depth = tex(NVSDK_NGX_Parameter_Depth,
                      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depth_state);
    frame.motion = tex(NVSDK_NGX_Parameter_MotionVectors,
                       NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                       NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, mv_state);
    frame.specular_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, read_state);
    frame.specular_ray_direction_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, read_state);
    frame.output = tex(NVSDK_NGX_Parameter_Output,
                       NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, output_state);

    frame.transparency_layer = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles,
                                   "DLSS.TransparencyLayer.Subrect.Base.X",
                                   "DLSS.TransparencyLayer.Subrect.Base.Y", read_state);
    if (!frame.transparency_layer.resource)
        frame.transparency_layer = tex("DLSS.TransparencyLayer",
                                       "DLSS.TransparencyLayer.Subrect.Base.X",
                                       "DLSS.TransparencyLayer.Subrect.Base.Y", read_state);
    frame.transparency_opacity = tex("DLSS.TransparencyLayerOpacity",
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.X",
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", read_state);
    frame.disocclusion_mask = tex("DLSS.DisocclusionMask",
                                  "DLSS.DisocclusionMask.Subrect.Base.X",
                                  "DLSS.DisocclusionMask.Subrect.Base.Y", read_state);
    frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, nullptr, nullptr, read_state);
    if (!frame.sss_guide.resource)
        frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, nullptr, nullptr, read_state);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;

    frame.responsivity_mask = tex("DLSSD.ResponsivityMask",
                                  "DLSSD.ResponsivityMask.Subrect.Base.X",
                                  "DLSSD.ResponsivityMask.Subrect.Base.Y", read_state);
    frame.color_before_transparency = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr, nullptr, read_state);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;

    frame.depth_of_field_guide = tex(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr, nullptr, read_state);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;

    frame.specular_motion = tex(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, nullptr, nullptr, read_state);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;

    frame.transparency_motion = tex("DLSS.TransparencyLayerMvecs",
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.X",
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", read_state);

    frame.disocclusion_mask_scale = cfg.disocclusion_mask_scale > 0.0f ? cfg.disocclusion_mask_scale : 1.0f;
    if (!cfg.use_disocclusion_mask) frame.disocclusion_mask = {};
    if (!cfg.use_sss_guide) frame.sss_guide = {};
    if (!cfg.use_responsivity_mask) frame.responsivity_mask = {};
    if (!cfg.use_color_before_transparency) frame.color_before_transparency = {};
    if (!cfg.use_depth_of_field_guide) frame.depth_of_field_guide = {};
    if (!cfg.use_reflection_motion_vectors) frame.specular_motion = {};
    if (!cfg.use_layer_opacity_and_motion) frame.transparency_opacity = frame.transparency_motion = {};

    log_d3d12_ngx_inputs(ctx, params, frame);

    if (!frame.color.resource || !frame.depth.resource || !frame.motion.resource || !frame.output.resource) {
        log("[openrr] D3D12 evaluate missing required resource (color=%p depth=%p motion=%p output=%p)",
            frame.color.resource, frame.depth.resource, frame.motion.resource, frame.output.resource);
        return false;
    }

    if (!ctx.guides_logged) {
        ctx.guides_logged = true;
        log("[openrr] guides diffuse_albedo=%d specular_albedo=%d normals=%d roughness=%d hit=%d dir_hit=%d(not read) tl=%d tl_op=%d tl_mv=%d dm=%d sss=%d resp=%d cbt=%d dof=%d smv=%d",
            int(frame.diffuse_albedo.resource != nullptr), int(frame.specular_albedo.resource != nullptr),
            int(frame.normals.resource != nullptr), int(frame.roughness.resource != nullptr),
            int(frame.specular_hit_distance.resource != nullptr), int(frame.specular_ray_direction_hit_distance.resource != nullptr),
            int(frame.transparency_layer.resource != nullptr), int(frame.transparency_opacity.resource != nullptr),
            int(frame.transparency_motion.resource != nullptr), int(frame.disocclusion_mask.resource != nullptr),
            int(frame.sss_guide.resource != nullptr), int(frame.responsivity_mask.resource != nullptr),
            int(frame.color_before_transparency.resource != nullptr), int(frame.depth_of_field_guide.resource != nullptr),
            int(frame.specular_motion.resource != nullptr));
    }

    int rough_mode = -1;
    if (params.Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &rough_mode) == NVSDK_NGX_Result_Success) {
        ctx.roughness_packed = (rough_mode == NVSDK_NGX_DLSS_Roughness_Mode_Packed) ? 1 : 0;
    } else if (ctx.roughness_packed < 0) {
        ctx.roughness_packed = frame.roughness.resource == nullptr ? 1 : 0;
    }

    int depth_type = -1;
    if (params.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth_type) == NVSDK_NGX_Result_Success) {
        ctx.has_ngx_depth_type = true;
        ctx.ngx_hw_depth = (depth_type == NVSDK_NGX_DLSS_Depth_Type_HW);
    }

    bool hw_depth = true;
    const DepthKind dkind = classify_d3d12_depth(static_cast<ID3D12Resource*>(frame.depth.resource));
    if (cfg.hardware_depth.has_value()) hw_depth = cfg.hardware_depth.value();
    else if (dkind == DepthKind::Hardware) hw_depth = true;
    else if (ctx.has_ngx_depth_type) hw_depth = ctx.ngx_hw_depth;
    else hw_depth = (dkind != DepthKind::GameWritten);

    if (int(hw_depth) != ctx.logged_hw_depth) {
        ctx.logged_hw_depth = int(hw_depth);
        log("[openrr] depth is %s (%s), %s",
            hw_depth ? "hardware" : "linear",
            ctx.has_ngx_depth_type ? (ctx.ngx_hw_depth ? "NGX says hardware" : "NGX says linear") : "NGX says nothing",
            depth_inverted ? "reversed Z" : "standard Z");
    }

    const ULONGLONG now_tick = GetTickCount64();
    const bool stale_history = ctx.last_eval_tick != 0 && (now_tick - ctx.last_eval_tick) > 250;
    ctx.last_eval_tick = now_tick;

    uint32_t reset = 0;
    if ((params.Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success && reset != 0) || stale_history)
        frame.flags |= ONR_RR_FRAME_RESET;
    if (hw_depth) frame.flags |= ONR_RR_FRAME_HW_DEPTH;
    if (depth_inverted) frame.flags |= ONR_RR_FRAME_DEPTH_INVERTED;
    if ((ctx.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0) frame.flags |= ONR_RR_FRAME_MV_JITTERED;
    if (ctx.roughness_packed == 1) frame.flags |= ONR_RR_FRAME_ROUGHNESS_PACKED;
    if (cfg.invert_disocclusion_mask) frame.flags |= ONR_RR_FRAME_DM_INVERT;
    if (cfg.clamp_disocclusion_mask) frame.flags |= ONR_RR_FRAME_DM_CLAMP;

    return true;
}

bool fill_vk_frame(RrContext& ctx, const NVSDK_NGX_Parameter& params, OnrRrFrame& frame) {
    const openrr::pe::Config cfg = openrr::pe::current_config();
    frame = {};
    frame.size = sizeof(frame);
    ctx.num_vk_images = 0;

    read_eval_render_size(&params, ctx.max_render_w, ctx.max_render_h, frame.render_width, frame.render_height);

    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &frame.jitter_x);
    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &frame.jitter_y);
    if (!std::isfinite(frame.jitter_x)) frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y)) frame.jitter_y = 0.0f;

    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.mv_scale_x) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.mv_scale_y) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_y) || frame.mv_scale_y == 0.0f)
        frame.mv_scale_y = 1.0f;

    const bool depth_inverted = (ctx.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    openrr::camera::ResolveCameraMatrices(params, depth_inverted, frame.world_to_view, frame.view_to_clip);

    const uint32_t read_layout = (cfg.vk_input_layout == "general")
                                     ? VK_IMAGE_LAYOUT_GENERAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const uint32_t output_layout = VK_IMAGE_LAYOUT_GENERAL;

    auto tex = [&](const char* key, const char* bx, const char* by, uint32_t layout) {
        OnrRrTexture t{};
        NVSDK_NGX_Resource_VK* res = get_vk_res(params, key, nullptr);
        if (!res || res->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
            !res->Resource.ImageViewInfo.Image || ctx.num_vk_images >= int(std::size(ctx.vk_images)))
            return t;
        const auto& v = res->Resource.ImageViewInfo;
        OnrRrVkImage& img = ctx.vk_images[ctx.num_vk_images++];
        img = {reinterpret_cast<uint64_t>(v.Image),
               reinterpret_cast<uint64_t>(v.ImageView),
               uint32_t(v.Format),
               v.Width,
               v.Height,
               v.SubresourceRange.baseMipLevel,
               v.SubresourceRange.baseArrayLayer,
               uint32_t(VK_IMAGE_USAGE_SAMPLED_BIT | (res->ReadWrite ? VK_IMAGE_USAGE_STORAGE_BIT : 0))};
        t.resource = &img;
        t.state = layout;
        if (bx) params.Get(bx, &t.x);
        if (by) params.Get(by, &t.y);
        return t;
    };

    frame.color = tex(NVSDK_NGX_Parameter_Color,
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, read_layout);
    frame.diffuse_albedo = tex(NVSDK_NGX_Parameter_DiffuseAlbedo,
                               NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                               NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read_layout);
    frame.specular_albedo = tex(NVSDK_NGX_Parameter_SpecularAlbedo,
                                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read_layout);
    frame.normals = tex(NVSDK_NGX_Parameter_GBuffer_Normals,
                        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read_layout);
    frame.roughness = tex(NVSDK_NGX_Parameter_GBuffer_Roughness,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read_layout);
    frame.depth = tex(NVSDK_NGX_Parameter_Depth,
                      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, read_layout);
    frame.motion = tex(NVSDK_NGX_Parameter_MotionVectors,
                       NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                       NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, read_layout);
    frame.specular_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, read_layout);
    frame.specular_ray_direction_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, read_layout);
    frame.output = tex(NVSDK_NGX_Parameter_Output,
                       NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, output_layout);

    frame.transparency_layer = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles,
                                   "DLSS.TransparencyLayer.Subrect.Base.X",
                                   "DLSS.TransparencyLayer.Subrect.Base.Y", read_layout);
    if (!frame.transparency_layer.resource)
        frame.transparency_layer = tex("DLSS.TransparencyLayer",
                                       "DLSS.TransparencyLayer.Subrect.Base.X",
                                       "DLSS.TransparencyLayer.Subrect.Base.Y", read_layout);
    frame.transparency_opacity = tex("DLSS.TransparencyLayerOpacity",
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.X",
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", read_layout);
    frame.disocclusion_mask = tex("DLSS.DisocclusionMask",
                                  "DLSS.DisocclusionMask.Subrect.Base.X",
                                  "DLSS.DisocclusionMask.Subrect.Base.Y", read_layout);
    frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, nullptr, nullptr, read_layout);
    if (!frame.sss_guide.resource)
        frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide, nullptr, nullptr, read_layout);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;

    frame.responsivity_mask = tex("DLSSD.ResponsivityMask",
                                  "DLSSD.ResponsivityMask.Subrect.Base.X",
                                  "DLSSD.ResponsivityMask.Subrect.Base.Y", read_layout);
    frame.color_before_transparency = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr, nullptr, read_layout);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;

    frame.depth_of_field_guide = tex(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr, nullptr, read_layout);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;

    frame.specular_motion = tex(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, nullptr, nullptr, read_layout);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;

    frame.transparency_motion = tex("DLSS.TransparencyLayerMvecs",
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.X",
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", read_layout);

    frame.disocclusion_mask_scale = cfg.disocclusion_mask_scale > 0.0f ? cfg.disocclusion_mask_scale : 1.0f;
    if (!cfg.use_disocclusion_mask) frame.disocclusion_mask = {};
    if (!cfg.use_sss_guide) frame.sss_guide = {};
    if (!cfg.use_responsivity_mask) frame.responsivity_mask = {};
    if (!cfg.use_color_before_transparency) frame.color_before_transparency = {};
    if (!cfg.use_depth_of_field_guide) frame.depth_of_field_guide = {};
    if (!cfg.use_reflection_motion_vectors) frame.specular_motion = {};
    if (!cfg.use_layer_opacity_and_motion) frame.transparency_opacity = frame.transparency_motion = {};

    if (!frame.color.resource || !frame.depth.resource || !frame.motion.resource || !frame.output.resource) {
        log("[openrr] Vulkan evaluate missing required resource");
        return false;
    }

    int rough_mode = -1;
    if (params.Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &rough_mode) == NVSDK_NGX_Result_Success) {
        ctx.roughness_packed = (rough_mode == NVSDK_NGX_DLSS_Roughness_Mode_Packed) ? 1 : 0;
    } else if (ctx.roughness_packed < 0) {
        ctx.roughness_packed = frame.roughness.resource == nullptr ? 1 : 0;
    }

    int depth_type = -1;
    if (params.Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth_type) == NVSDK_NGX_Result_Success) {
        ctx.has_ngx_depth_type = true;
        ctx.ngx_hw_depth = (depth_type == NVSDK_NGX_DLSS_Depth_Type_HW);
    }

    const auto* depth_img = static_cast<const OnrRrVkImage*>(frame.depth.resource);
    bool hw_depth = true;
    if (cfg.hardware_depth.has_value()) hw_depth = cfg.hardware_depth.value();
    else if (is_vk_depth_format(VkFormat(depth_img->format))) hw_depth = true;
    else if (ctx.has_ngx_depth_type) hw_depth = ctx.ngx_hw_depth;

    const ULONGLONG now_tick_vk = GetTickCount64();
    const bool stale_history_vk = ctx.last_eval_tick != 0 && (now_tick_vk - ctx.last_eval_tick) > 250;
    ctx.last_eval_tick = now_tick_vk;

    uint32_t reset = 0;
    if ((params.Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success && reset != 0) || stale_history_vk)
        frame.flags |= ONR_RR_FRAME_RESET;
    if (hw_depth) frame.flags |= ONR_RR_FRAME_HW_DEPTH;
    if (depth_inverted) frame.flags |= ONR_RR_FRAME_DEPTH_INVERTED;
    if ((ctx.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0) frame.flags |= ONR_RR_FRAME_MV_JITTERED;
    if (ctx.roughness_packed == 1) frame.flags |= ONR_RR_FRAME_ROUGHNESS_PACKED;
    if (cfg.invert_disocclusion_mask) frame.flags |= ONR_RR_FRAME_DM_INVERT;
    if (cfg.clamp_disocclusion_mask) frame.flags |= ONR_RR_FRAME_DM_CLAMP;

    return true;
}

void update_status_from_context(const RrContext& ctx) {
    OnrRrInfo info{};
    info.size = sizeof(info);
    if (g_api.GetInfo(ctx.runtime_ctx, &info) != ONR_RR_OK) return;

    if (info.state == ONR_RR_STATE_FAILED) {
        block_session(std::string("runtime failed: ") + info.message);
    }

    openrr::pe::control::Status st{};
    st.route = ctx.is_vk ? 2u : 1u;
    st.rr_active = true;
    st.state = info.state;
    st.backend = info.backend;
    st.fast_mode = uint32_t(OnrRrGetFastMode(ctx.runtime_ctx));
    st.frames = info.frames;
    st.gpu_ms_last = info.gpu_ms_last;
    st.gpu_ms_average = info.gpu_ms_average;
    OnrRrGetGpuTiming(ctx.runtime_ctx, &st.gpu_raw_last, &st.gpu_raw_p50, &st.gpu_raw_p95, &st.gpu_ms_last, &st.gpu_ms_average);
    st.render_width = info.render_width ? info.render_width : ctx.max_render_w;
    st.render_height = info.render_height ? info.render_height : ctx.max_render_h;
    st.output_width = info.output_width ? info.output_width : ctx.output_w;
    st.output_height = info.output_height ? info.output_height : ctx.output_h;
    st.coexist_mochi = coexist_mochi_detected();
    st.weights_source = info.weights_source;
    st.message = info.message;
    st.backend_message = info.backend_message;
    openrr::pe::control::report(st);
}

// ---- Native OptiScaler DLSSD Provider (OptiProvider_Dlssd) ----
void sync_watchdog_rr_active_locked() {
    openrr::pe::watchdog::set_rr_active(!g_d3d12_rr.empty() || !g_vk_rr.empty());
}

void on_d3d12_device_seen(ID3D12Device* dev) {
    ensure_cmd_sig_hook(dev);
    ensure_deferred_init();
    if (!dev) return;
    const bool is_vkd3d = openrr::pe::vkd3d::is_vkd3d_device(dev);
    if (is_vkd3d) rr_vk_use_loader(0);
    const LUID luid = dev->GetAdapterLuid();
    uint64_t luid_bits = 0;
    std::memcpy(&luid_bits, &luid, sizeof(luid));
    g_last_d3d12_luid_bits.store(luid_bits, std::memory_order_release);
    g_has_d3d12_luid.store(true, std::memory_order_release);
    if (check_usable(&luid)) {
        const openrr::pe::Config cfg = openrr::pe::current_config();
        if (!is_vkd3d && cfg.backend == "vulkan" && split_allowed_by_config()) {
            ensure_allocator_reset_hook(dev);
        } else {
            ensure_direct_state_hooks(dev, nullptr);
        }
    }
}

NVSDK_NGX_Result provider_d3d12_init_impl(ID3D12Device* dev, const NVSDK_NGX_FeatureCommonInfo* info) {
    note_feature_common_info(info);
    on_d3d12_device_seen(dev);
    if (!dev) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const LUID luid = dev->GetAdapterLuid();
    if (!check_usable(&luid)) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_init(unsigned long long, const wchar_t*, ID3D12Device* dev,
                                                  const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version) {
    try { return provider_d3d12_init_impl(dev, info); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_init_ext(unsigned long long, const wchar_t*, ID3D12Device* dev,
                                                      NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo* info) {
    try { return provider_d3d12_init_impl(dev, info); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_populate_params(NVSDK_NGX_Parameter* params) {
    try {
        ensure_deferred_init();
        if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if (!d3d12_rr_capable()) {
            params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 0);
            params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult,
                        static_cast<int>(NVSDK_NGX_Result_FAIL_FeatureNotSupported));
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 1);
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, 0);
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, 1);
        populate_dlssd_callbacks(params);
        return NVSDK_NGX_Result_Success;
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_get_feat_req(IDXGIAdapter* adapter,
                                                          const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                                          NVSDK_NGX_FeatureRequirement* out) {
    try {
        ensure_deferred_init();
        if (!info || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        note_feature_common_info(info->FeatureInfo);
        if (info->FeatureID != NVSDK_NGX_Feature_RayReconstruction) {
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        LUID luid{};
        const LUID* p_luid = nullptr;
        if (adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                luid = desc.AdapterLuid;
                p_luid = &luid;
            }
        }
        if (p_luid ? check_usable(p_luid) : d3d12_rr_capable()) {
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
            out->MinHWArchitecture = 0;
            std::snprintf(out->MinOSVersion, sizeof(out->MinOSVersion), "10.0.10240.16384");
            return NVSDK_NGX_Result_Success;
        }
        out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_get_scratch(NVSDK_NGX_Feature feat, const NVSDK_NGX_Parameter*,
                                                         size_t* out) {
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (feat != NVSDK_NGX_Feature_RayReconstruction) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    *out = 0;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result create_d3d12_rr(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                                 NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    if (feature != NVSDK_NGX_Feature_RayReconstruction) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    if (!cmd || !params || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const RrCreateParams create = read_create_params(*params);
    ensure_deferred_init();

    ID3D12Device* dev = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    struct DevHold { ID3D12Device* d; ~DevHold() { if (d) d->Release(); } } hold{dev};
    on_d3d12_device_seen(dev);

    const LUID luid = dev->GetAdapterLuid();
    if (!check_usable(&luid)) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }

    const openrr::pe::Config cfg = openrr::pe::current_config();

    // Re-probe mochi coexistence at Create time in case dlssnr-amd-loadlate.asi finished loading (Item #7).
    const bool mochi = probe_coexist_mochi();
    const bool is_vkd3d = openrr::pe::vkd3d::is_vkd3d_device(dev);

    // Only install command-list split hooks when the Vulkan interop bridge is requested;
    // D3D12 Direct records straight into the game's list without splitting (HANDOFF §3.6, Item #1),
    // and vkd3d-proton always uses RecordVk interop directly.
    if (!is_vkd3d && cfg.backend == "vulkan" && split_allowed_by_config()) {
        ensure_allocator_reset_hook(dev);
        std::string why;
        openrr::pe::split::prepare(cmd, &why);
    } else {
        ensure_direct_state_hooks(dev, cmd);
    }

    auto ctx = std::make_unique<RrContext>();
    ctx->is_vk = false;
    ctx->d3d12_device = dev;

    if (!create.max_render_w || !create.max_render_h) {
        log("[openrr] D3D12 CreateFeature: invalid dimensions (%ux%u)", create.max_render_w, create.max_render_h);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    apply_create_params(*ctx, create);

    const std::wstring data_dir = resolve_data_dir();
    const std::wstring log_path = resolve_log_path();
    std::wstring dlssd;
    {
        std::lock_guard<std::mutex> lock(g_rt_mutex);
        dlssd = g_dlssd_path;
    }

    std::shared_ptr<openrr::pe::vkd3d::InteropSession> vkd3d_session;
    OnrRrVkDevice vkd_info{};
    if (is_vkd3d) {
        auto sess = std::make_shared<openrr::pe::vkd3d::InteropSession>();
        std::string vkd3d_why;
        if (!openrr::pe::vkd3d::probe_and_open(dev, *sess, vkd3d_why)) {
            log("[openrr] vkd3d-proton interop unavailable: %s", vkd3d_why.c_str());
            block_session("vkd3d-proton interop unavailable: " + vkd3d_why);
            return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        }
        vkd_info = {sizeof(vkd_info), sess->api_version, sess->instance, sess->physical, sess->device,
                    sess->queue_family, sess->queue_index};
        vkd3d_session = std::move(sess);
    }

    // Coexistence rule (HANDOFF §4): when dlssnr-amd*.asi is loaded, Backend=auto chooses D3D12 Direct (no split).
    uint32_t backend_choice = ONR_RR_BACKEND_CHOICE_AUTO;
    if (cfg.backend == "vulkan" && !mochi && cfg.coexist_split != "off") {
        backend_choice = ONR_RR_BACKEND_CHOICE_VULKAN;
    } else if (cfg.backend == "d3d12" || mochi || cfg.coexist_split == "off") {
        backend_choice = ONR_RR_BACKEND_CHOICE_D3D12;
    }

    OnrRrCreateInfo ci{};
    ci.size = sizeof(ci);
    ci.mode = ONR_RR_MODE_FAST;
    ci.d3d12_device = vkd3d_session ? nullptr : dev;
    ci.max_render_width = ctx->max_render_w;
    ci.max_render_height = ctx->max_render_h;
    ci.output_width = ctx->output_w;
    ci.output_height = ctx->output_h;
    ci.dlssd_path = dlssd.c_str();
    ci.data_dir = data_dir.c_str();
    ci.log_path = log_path.c_str();
    ci.backend = vkd3d_session ? uint32_t(ONR_RR_BACKEND_CHOICE_AUTO) : backend_choice;
    ci.vk_device = vkd3d_session ? &vkd_info : nullptr;

    openrr::pe::vulkan_entry_points();
    int32_t rc = ONR_RR_FAILED;
    {
        InternalVkScope vk_scope;
        ++t_in_openrr_record;
        rc = g_api.Create(&ci, &ctx->runtime_ctx);
        --t_in_openrr_record;
    }
    if (rc != ONR_RR_OK || !ctx->runtime_ctx) {
        const std::string err = runtime_last_error();
        log("[openrr] D3D12 Create failed (rc=%d): %s", rc, err.c_str());
        block_session("Create failed: " + err);
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    struct CreateCleanup {
        RrContext* ctx;
        bool committed = false;
        ~CreateCleanup() {
            if (!committed && ctx) {
                if (ctx->runtime_ctx) {
                    InternalVkScope vk_scope;
                    g_api.Destroy(ctx->runtime_ctx);
                    ctx->runtime_ctx = nullptr;
                }
                if (ctx->holder) ctx->holder->ctx = nullptr;
                if (ctx->allocated_handle) {
                    delete ctx->allocated_handle;
                    ctx->allocated_handle = nullptr;
                }
            }
        }
    } cleanup{ctx.get()};
    ctx->holder->ctx = ctx->runtime_ctx;

    ctx->mode_applied = cfg.mode;
    OnrRrSetFastMode(ctx->runtime_ctx, cfg.mode);

    const unsigned int id = g_next_rr_handle.fetch_add(1);
    ctx->handle_id = id;
    if (*out == nullptr) {
        ctx->allocated_handle = new NVSDK_NGX_Handle{id};
    }

    uint32_t running_be = ONR_RR_BACKEND_D3D12;
    if (g_api4.GetBackend) g_api4.GetBackend(ctx->runtime_ctx, &running_be);
    if (!vkd3d_session && running_be == ONR_RR_BACKEND_VULKAN && split_allowed_by_config()) {
        std::string why;
        openrr::pe::split::prepare(cmd, &why);
    }

    log("[openrr] D3D12 Ray Reconstruction feature created (handle=0x%X, render=%ux%u -> output=%ux%u, backend=%s, coexist_mochi=%s)",
        id, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h,
        vkd3d_session ? "vkd3d-proton Vulkan Host"
                      : (running_be == ONR_RR_BACKEND_D3D12 ? "D3D12 Direct" : "Vulkan Bridge"),
        mochi ? "yes" : "no");

    update_status_from_context(*ctx);
    NVSDK_NGX_Handle* allocated_out = ctx->allocated_handle;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        if (vkd3d_session) g_vkd3d_sessions[id] = std::move(vkd3d_session);
        g_d3d12_rr[id] = std::move(ctx);
        sync_watchdog_rr_active_locked();
    }
    cleanup.committed = true;
    if (allocated_out) {
        *out = allocated_out;
    } else {
        (*out)->Id = id;
    }
    return NVSDK_NGX_Result_Success;
}

std::atomic<uint64_t> g_d3d_skipped_frames{0};

NVSDK_NGX_Result evaluate_vkd3d_interop_rr(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params,
                                           RrContext& ctx, openrr::pe::vkd3d::InteropSession& session,
                                           std::unique_lock<std::mutex> lock) {
    if (!cmd || !params || !ctx.runtime_ctx || !g_api5.RecordVk) {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    openrr::pe::watchdog::beat();

    OnrRrFrame d3d_frame{};
    if (!fill_d3d12_frame(ctx, *params, d3d_frame)) {
        return NVSDK_NGX_Result_Fail;
    }

    OnrRrFrame vk_frame = d3d_frame;
    vk_frame.size = sizeof(vk_frame);
    OnrRrVkImage vk_imgs[19]{};
    struct TexPair {
        const OnrRrTexture* src;
        OnrRrTexture* dst;
    };
    const TexPair pairs[19] = {
        {&d3d_frame.color, &vk_frame.color},
        {&d3d_frame.diffuse_albedo, &vk_frame.diffuse_albedo},
        {&d3d_frame.specular_albedo, &vk_frame.specular_albedo},
        {&d3d_frame.normals, &vk_frame.normals},
        {&d3d_frame.roughness, &vk_frame.roughness},
        {&d3d_frame.depth, &vk_frame.depth},
        {&d3d_frame.motion, &vk_frame.motion},
        {&d3d_frame.specular_hit_distance, &vk_frame.specular_hit_distance},
        {&d3d_frame.specular_ray_direction_hit_distance, &vk_frame.specular_ray_direction_hit_distance},
        {&d3d_frame.output, &vk_frame.output},
        {&d3d_frame.transparency_layer, &vk_frame.transparency_layer},
        {&d3d_frame.transparency_opacity, &vk_frame.transparency_opacity},
        {&d3d_frame.disocclusion_mask, &vk_frame.disocclusion_mask},
        {&d3d_frame.sss_guide, &vk_frame.sss_guide},
        {&d3d_frame.responsivity_mask, &vk_frame.responsivity_mask},
        {&d3d_frame.color_before_transparency, &vk_frame.color_before_transparency},
        {&d3d_frame.depth_of_field_guide, &vk_frame.depth_of_field_guide},
        {&d3d_frame.specular_motion, &vk_frame.specular_motion},
        {&d3d_frame.transparency_motion, &vk_frame.transparency_motion},
    };
    for (size_t i = 0; i < std::size(pairs); ++i) {
        if (!openrr::pe::vkd3d::fill_vk_image(session, *pairs[i].src, vk_imgs[i], *pairs[i].dst)) {
            log("[openrr] vkd3d-proton: failed to extract Vulkan image for guide slot %zu", i);
            return NVSDK_NGX_Result_Fail;
        }
    }

    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.mode != ctx.mode_applied) {
        ctx.mode_applied = cfg.mode;
        OnrRrSetFastMode(ctx.runtime_ctx, cfg.mode);
    }

    openrr::pe::vkd3d::ScopedCmdInterop cmd_scope(session, cmd);
    if (!cmd_scope.vk_cmd) {
        log("[openrr] vkd3d-proton: failed to obtain VkCommandBuffer for command list %p", static_cast<void*>(cmd));
        return NVSDK_NGX_Result_Fail;
    }

    int32_t rc = ONR_RR_FAILED;
    {
        InternalVkScope vk_scope;
        rc = g_api5.RecordVk(ctx.runtime_ctx, &vk_frame, cmd_scope.vk_cmd);
    }
    if (rc == ONR_RR_BUSY) {
        const uint64_t skip_n = ++g_d3d_skipped_frames;
        if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
            log("[openrr] vkd3d-proton: all 64 recording slots in flight; skipping frame (count=%llu)",
                static_cast<unsigned long long>(skip_n));
        }
        return NVSDK_NGX_Result_Success;
    }
    if (rc != ONR_RR_OK) {
        update_status_from_context(ctx);
        log("[openrr] vkd3d-proton RecordVk failed (rc=%d): %s", rc, runtime_last_error().c_str());
        return NVSDK_NGX_Result_Fail;
    }
    ++ctx.frame_count;
    if (ctx.frame_count <= 32 || ctx.frame_count % 30 == 0) {
        update_status_from_context(ctx);
    }
    (void)lock;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result evaluate_d3d12_rr(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
                                   NVSDK_NGX_Parameter* params) {
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    std::unique_lock<std::mutex> lock(g_ctx_mutex);
    auto it = g_d3d12_rr.find(handle->Id);
    if (it == g_d3d12_rr.end()) {
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    if (auto vit = g_vkd3d_sessions.find(handle->Id); vit != g_vkd3d_sessions.end() && vit->second) {
        auto sess = vit->second;
        return evaluate_vkd3d_interop_rr(cmd, params, *it->second, *sess, std::move(lock));
    }
    if (!cmd || !params) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    openrr::pe::watchdog::beat();

    RrContext& ctx = *it->second;
    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.mode != ctx.mode_applied) {
        ctx.mode_applied = cfg.mode;
        OnrRrSetFastMode(ctx.runtime_ctx, cfg.mode);
    }

    OnrRrFrame frame{};
    if (!fill_d3d12_frame(ctx, *params, frame)) {
        return NVSDK_NGX_Result_Fail;
    }

    uint32_t running_be = ONR_RR_BACKEND_D3D12;
    if (g_api4.GetBackend) g_api4.GetBackend(ctx.runtime_ctx, &running_be);

    const bool can_split = (running_be == ONR_RR_BACKEND_VULKAN) && split_allowed_by_config();
    bool split_ok = false;
    bool direct_hooks_ok = false;
    std::string split_why;
    if (can_split) {
        split_ok = openrr::pe::split::prepare(cmd, &split_why);
    } else {
        direct_hooks_ok = ensure_direct_state_hooks(ctx.d3d12_device, cmd);
    }

    // When Vulkan interop bridge is running and this command list was already Reset before split hooks
    // were installed, skip at most 2 frames so the next Reset is tracked cleanly; otherwise fail cleanly.
    if (running_be == ONR_RR_BACKEND_VULKAN && !split_ok) {
        if (can_split && !openrr::pe::split::disabled() &&
            openrr::pe::split::is_untracked_reset_reason(split_why) && ++ctx.unknown_streak <= 2) {
            const uint64_t skip_n = ++g_d3d_skipped_frames;
            log("[openrr] command list state before Reset not yet tracked (%s, streak=%u); skipping this frame (output not written, count=%llu)",
                split_why.c_str(), ctx.unknown_streak, static_cast<unsigned long long>(skip_n));
            return NVSDK_NGX_Result_Success;
        }
        static std::atomic<bool> logged_fallback{false};
        if (!logged_fallback.exchange(true, std::memory_order_relaxed)) {
            log("[openrr] Vulkan bridge cannot split command list (%s)",
                can_split ? split_why.c_str() : "disabled by coexistence rule");
        }
        return NVSDK_NGX_Result_Fail;
    }

    if (running_be == ONR_RR_BACKEND_D3D12) {
        if (!split_ok) {
            if (!direct_hooks_ok) {
                log("[openrr] D3D12 Direct: state hooks unavailable; failing evaluation");
                return NVSDK_NGX_Result_Fail;
            }
            const DirectListState* st = direct_state_of(cmd);
            if (!st || !st->valid || st->unknown) {
                const uint32_t streak = ++ctx.unknown_streak;
                if (streak <= 2) {
                    const uint64_t skip_n = ++g_d3d_skipped_frames;
                    if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
                        log("[openrr] D3D12 Direct: command-list state is not known yet (%s, streak=%u); skipping this frame (output not written, count=%llu)",
                            (!st || !st->valid) ? "recorded before Reset hook" : "bundle state unknown",
                            streak, static_cast<unsigned long long>(skip_n));
                    }
                    return NVSDK_NGX_Result_Success;
                }
                if (streak == 3) {
                    log("[openrr] D3D12 Direct: command-list state remained untracked for %u frames; failing evaluation", streak);
                }
                return NVSDK_NGX_Result_Fail;
            }
            ctx.unknown_streak = 0;
        }
        bool just_became_ready = false;
        if (!ctx.ready_logged && g_api.GetInfo) {
            OnrRrInfo pre_info{};
            pre_info.size = sizeof(pre_info);
            if (g_api.GetInfo(ctx.runtime_ctx, &pre_info) == ONR_RR_OK && pre_info.state == ONR_RR_STATE_READY) {
                ctx.ready_logged = true;
                just_became_ready = true;
                log("[openrr] D3D12 runtime transitioned to READY on frame %llu; recording first full network frame",
                    static_cast<unsigned long long>(ctx.frame_count));
            }
        }
        if (just_became_ready || ctx.frame_count < 4) {
            openrr::pe::flush_log();
        }
        const auto state = split_ok ? openrr::pe::split::snapshot(cmd) : nullptr;
        bool host_bound = false;
        const bool trace = openrr::pe::d3d12_diagnostics_enabled() && (ctx.frame_count < 4 || just_became_ready);
        if (trace) {
            log("[d3d12 diag] Evaluate list=%p next_frame=%llu", static_cast<void*>(cmd),
                static_cast<unsigned long long>(ctx.frame_count + 1));
            log_direct_texture_barriers(cmd, frame);
        }
        ++t_in_openrr_record;
        const int32_t rc = openrr::pe::record_d3d12(ctx.runtime_ctx, &frame, cmd, host_bound, trace);
        --t_in_openrr_record;
        const bool device_removed = (rc != ONR_RR_OK && rc != ONR_RR_BUSY) &&
                                    openrr::pe::log_d3d12_device_removed(ctx.d3d12_device, "Record");
        bool restored = false;
        if (host_bound && !device_removed) {
            if (split_ok && state) {
                openrr::pe::split::restore(cmd, state);
                restored = true;
            } else {
                restored = restore_direct_state(cmd);
            }
        }
        if (trace) {
            log("[d3d12 diag] list=%p Record rc=%d host_bound=%d restore=%d",
                static_cast<void*>(cmd), rc, int(host_bound), int(restored));
        }
        if (trace || just_became_ready || ctx.frame_count < 4) openrr::pe::flush_log();
        if (rc == ONR_RR_BUSY) {
            const uint64_t skip_n = ++g_d3d_skipped_frames;
            if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
                log("[openrr] D3D12 Direct: all 127 recording slots in flight; skipping this frame (output not written, count=%llu)",
                    static_cast<unsigned long long>(skip_n));
            }
            return NVSDK_NGX_Result_Success;
        }
        if (rc != ONR_RR_OK) {
            update_status_from_context(ctx);
            log("[openrr] D3D12 Record failed (rc=%d): %s", rc, runtime_last_error().c_str());
            return NVSDK_NGX_Result_Fail;
        }
    } else {
        // Vulkan interop bridge via command-list split (RecordInputs -> cut(Enqueue) -> RecordOutput)
        const auto state = openrr::pe::split::snapshot(cmd);
        void* job = nullptr;
        const int32_t rc_in = g_api.RecordInputs(ctx.runtime_ctx, &frame, cmd, &job);
        if (rc_in != ONR_RR_OK || !job) {
            openrr::pe::split::restore(cmd, state);
            update_status_from_context(ctx);
            log("[openrr] D3D12 RecordInputs failed (rc=%d): %s", rc_in, runtime_last_error().c_str());
            return NVSDK_NGX_Result_Fail;
        }
        const auto holder = ctx.holder;
        const auto out_ok = std::make_shared<std::atomic<bool>>(false);
        std::string cut_why;
        openrr::pe::split::Job split_job{
            [holder, job, out_ok](ID3D12CommandQueue* q) {
                std::lock_guard<std::mutex> hlock(holder->mutex);
                if (!holder->ctx) return;
                if (out_ok->load(std::memory_order_acquire))
                    g_api.Enqueue(holder->ctx, job, q);
                else
                    g_api.Cancel(holder->ctx, job);
            },
            [holder, job]() {
                std::lock_guard<std::mutex> hlock(holder->mutex);
                if (holder->ctx) g_api.Cancel(holder->ctx, job);
            }};
        if (!openrr::pe::split::cut(cmd, std::move(split_job), state, &cut_why)) {
            g_api.Cancel(ctx.runtime_ctx, job);
            openrr::pe::split::restore(cmd, state);
            log("[openrr] D3D12 split cut refused (%s)", cut_why.c_str());
            return NVSDK_NGX_Result_Fail;
        }
        const int32_t rc_out = g_api.RecordOutput(ctx.runtime_ctx, job, cmd);
        if (rc_out != ONR_RR_OK) {
            update_status_from_context(ctx);
            log("[openrr] D3D12 RecordOutput failed (rc=%d): %s", rc_out, runtime_last_error().c_str());
            return NVSDK_NGX_Result_Fail;
        }
        out_ok->store(true, std::memory_order_release);
    }

    ++ctx.frame_count;
    if (ctx.frame_count <= 32 || ctx.frame_count % 30 == 0) {
        update_status_from_context(ctx);
    }
    return NVSDK_NGX_Result_Success;
}

void destroy_d3d12_rr_context(RrContext& ctx) {
    void* rt = nullptr;
    if (ctx.holder) {
        std::lock_guard<std::mutex> hlock(ctx.holder->mutex);
        rt = ctx.holder->ctx;
        ctx.holder->ctx = nullptr;
    } else {
        rt = ctx.runtime_ctx;
    }
    ctx.runtime_ctx = nullptr;
    if (rt) {
        InternalVkScope vk_scope;
        g_api.Destroy(rt);
    }
}

NVSDK_NGX_Result release_d3d12_rr(NVSDK_NGX_Handle* handle) {
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::unique_ptr<RrContext> gone;
    std::shared_ptr<openrr::pe::vkd3d::InteropSession> gone_vkd3d;
    bool any_active = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        auto it = g_d3d12_rr.find(handle->Id);
        if (it != g_d3d12_rr.end()) {
            gone = std::move(it->second);
            g_d3d12_rr.erase(it);
        }
        if (auto vit = g_vkd3d_sessions.find(handle->Id); vit != g_vkd3d_sessions.end()) {
            gone_vkd3d = std::move(vit->second);
            g_vkd3d_sessions.erase(vit);
        }
        any_active = !g_d3d12_rr.empty() || !g_vk_rr.empty();
        sync_watchdog_rr_active_locked();
    }
    if (!gone) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    log("[openrr] D3D12 Ray Reconstruction feature released (handle=0x%X)", gone->handle_id);
    destroy_d3d12_rr_context(*gone);
    gone_vkd3d.reset();
    if (gone->allocated_handle == handle) delete gone->allocated_handle;
    if (!any_active) {
        openrr::pe::control::Status st{};
        openrr::pe::control::report(st);
    }
    return NVSDK_NGX_Result_Success;
}

void destroy_all_d3d12_rr(ID3D12Device* filter_dev = nullptr) {
    std::vector<std::unique_ptr<RrContext>> list;
    std::vector<std::shared_ptr<openrr::pe::vkd3d::InteropSession>> vkd3d_list;
    bool any_active = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        for (auto it = g_d3d12_rr.begin(); it != g_d3d12_rr.end();) {
            if (!filter_dev || (it->second && it->second->d3d12_device == filter_dev)) {
                const unsigned int id = it->first;
                list.push_back(std::move(it->second));
                it = g_d3d12_rr.erase(it);
                if (auto vit = g_vkd3d_sessions.find(id); vit != g_vkd3d_sessions.end()) {
                    vkd3d_list.push_back(std::move(vit->second));
                    g_vkd3d_sessions.erase(vit);
                }
            } else {
                ++it;
            }
        }
        any_active = !g_d3d12_rr.empty() || !g_vk_rr.empty();
        sync_watchdog_rr_active_locked();
    }
    for (auto& ctx : list) {
        if (ctx) {
            destroy_d3d12_rr_context(*ctx);
            if (ctx->allocated_handle) delete ctx->allocated_handle;
        }
    }
    vkd3d_list.clear();
    if (!list.empty() && !any_active) {
        openrr::pe::control::Status st{};
        openrr::pe::control::report(st);
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_create(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                                                    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    try { return create_d3d12_rr(cmd, feature, params, out); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
                                                      const NVSDK_NGX_Parameter* params, void*) {
    try { return evaluate_d3d12_rr(cmd, handle, const_cast<NVSDK_NGX_Parameter*>(params)); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_release(NVSDK_NGX_Handle* handle) {
    try { return release_d3d12_rr(handle); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_shutdown() {
    try { destroy_all_d3d12_rr(); return NVSDK_NGX_Result_Success; }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_d3d12_shutdown1(ID3D12Device* dev) {
    try { destroy_all_d3d12_rr(dev); return NVSDK_NGX_Result_Success; }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

// ---- Vulkan Provider Implementation ----
std::mutex g_vk_ngx_mutex;
VkInstance g_ngx_vk_instance = VK_NULL_HANDLE;
VkDevice g_ngx_vk_device = VK_NULL_HANDLE;

void note_ngx_vk(VkInstance inst, VkDevice dev, const NVSDK_NGX_FeatureCommonInfo* info) {
    ensure_deferred_init();
    note_feature_common_info(info);
    std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
    if (inst) g_ngx_vk_instance = inst;
    if (dev) g_ngx_vk_device = dev;
}

NVSDK_NGX_Result provider_vk_init_impl(VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                                       const NVSDK_NGX_FeatureCommonInfo* info) {
    note_ngx_vk(inst, dev, info);
    if (!vk_rr_capable(phys, dev)) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_init(unsigned long long, const wchar_t*, VkInstance inst,
                                               VkPhysicalDevice phys, VkDevice dev,
                                               PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                               const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version) {
    try { return provider_vk_init_impl(inst, phys, dev, info); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_init_ext(unsigned long long, const wchar_t*, VkInstance inst,
                                                   VkPhysicalDevice phys, VkDevice dev, NVSDK_NGX_Version,
                                                   const NVSDK_NGX_FeatureCommonInfo* info) {
    try { return provider_vk_init_impl(inst, phys, dev, info); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_init_ext2(unsigned long long, const wchar_t*, VkInstance inst,
                                                    VkPhysicalDevice phys, VkDevice dev,
                                                    PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                                    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo* info) {
    try { return provider_vk_init_impl(inst, phys, dev, info); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_populate_params(NVSDK_NGX_Parameter* params) {
    try {
        ensure_deferred_init();
        if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if (!vk_rr_capable()) {
            params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 0);
            params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult,
                        static_cast<int>(NVSDK_NGX_Result_FAIL_FeatureNotSupported));
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 1);
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, 0);
        params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, 1);
        populate_dlssd_callbacks(params);
        return NVSDK_NGX_Result_Success;
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_get_feat_req(VkInstance, VkPhysicalDevice phys,
                                                       const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                                       NVSDK_NGX_FeatureRequirement* out) {
    try {
        ensure_deferred_init();
        if (!info || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        note_feature_common_info(info->FeatureInfo);
        if (info->FeatureID != NVSDK_NGX_Feature_RayReconstruction) {
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        if (vk_rr_capable(phys, VK_NULL_HANDLE, true)) {
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
            out->MinHWArchitecture = 0;
            std::snprintf(out->MinOSVersion, sizeof(out->MinOSVersion), "10.0.10240.16384");
            return NVSDK_NGX_Result_Success;
        }
        out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_get_scratch(NVSDK_NGX_Feature feat, const NVSDK_NGX_Parameter*,
                                                      size_t* out) {
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (feat != NVSDK_NGX_Feature_RayReconstruction) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    *out = 0;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result create_vk_rr_feature(VkDevice dev, const RrCreateParams& create, NVSDK_NGX_Handle** out) {
    ensure_deferred_init();

    openrr::pe::vkdevice::DeviceInfo di{};
    if (!openrr::pe::vkdevice::get_device_info(dev, &di) || !di.prepared) {
        log("[openrr] Vulkan CreateFeature: device %p was not prepared with runtime extensions/queue", static_cast<void*>(dev));
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    if (!di.instance) {
        std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
        di.instance = g_ngx_vk_instance;
    }
    if (!vk_rr_capable(di.physical, di.device)) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    probe_coexist_mochi();

    auto ctx = std::make_unique<RrContext>();
    ctx->is_vk = true;
    ctx->vk_device = di.device;

    if (!create.max_render_w || !create.max_render_h) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    apply_create_params(*ctx, create);

    const std::wstring data_dir = resolve_data_dir();
    const std::wstring log_path = resolve_log_path();
    std::wstring dlssd;
    {
        std::lock_guard<std::mutex> lock(g_rt_mutex);
        dlssd = g_dlssd_path;
    }

    OnrRrVkDevice vkd{};
    vkd.size = sizeof(vkd);
    vkd.api_version = di.api_version ? di.api_version : VK_API_VERSION_1_3;
    vkd.instance = di.instance;
    vkd.physical_device = di.physical;
    vkd.device = di.device;
    vkd.queue_family = di.queue_family;
    vkd.queue_index = di.queue_index;

    OnrRrCreateInfo ci{};
    ci.size = sizeof(ci);
    ci.mode = ONR_RR_MODE_FAST;
    ci.max_render_width = ctx->max_render_w;
    ci.max_render_height = ctx->max_render_h;
    ci.output_width = ctx->output_w;
    ci.output_height = ctx->output_h;
    ci.dlssd_path = dlssd.c_str();
    ci.data_dir = data_dir.c_str();
    ci.log_path = log_path.c_str();
    ci.vk_device = &vkd;

    openrr::pe::vulkan_entry_points();
    int32_t rc = ONR_RR_FAILED;
    {
        InternalVkScope vk_scope;
        ++t_in_openrr_record;
        rc = g_api.Create(&ci, &ctx->runtime_ctx);
        --t_in_openrr_record;
    }
    if (rc != ONR_RR_OK || !ctx->runtime_ctx) {
        const std::string err = runtime_last_error();
        log("[openrr] Vulkan Create failed (rc=%d): %s", rc, err.c_str());
        block_session("Vulkan Create failed: " + err);
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    struct VkCreateCleanup {
        RrContext* ctx;
        bool committed = false;
        ~VkCreateCleanup() {
            if (!committed && ctx) {
                if (ctx->runtime_ctx) {
                    InternalVkScope vk_scope;
                    g_api.Destroy(ctx->runtime_ctx);
                    ctx->runtime_ctx = nullptr;
                }
                if (ctx->allocated_handle) {
                    delete ctx->allocated_handle;
                    ctx->allocated_handle = nullptr;
                }
            }
        }
    } cleanup{ctx.get()};

    const openrr::pe::Config cfg = openrr::pe::current_config();
    ctx->mode_applied = cfg.mode;
    OnrRrSetFastMode(ctx->runtime_ctx, cfg.mode);

    const unsigned int id = g_next_rr_handle.fetch_add(1);
    ctx->handle_id = id;
    if (*out == nullptr) {
        ctx->allocated_handle = new NVSDK_NGX_Handle{id};
    }

    log("[openrr] Vulkan Ray Reconstruction feature created (handle=0x%X, render=%ux%u -> output=%ux%u)",
        id, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h);

    update_status_from_context(*ctx);
    NVSDK_NGX_Handle* allocated_out = ctx->allocated_handle;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_vk_rr[id] = std::move(ctx);
        sync_watchdog_rr_active_locked();
    }
    cleanup.committed = true;
    if (allocated_out) {
        *out = allocated_out;
    } else {
        (*out)->Id = id;
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_create1(VkDevice dev, VkCommandBuffer, NVSDK_NGX_Feature feature,
                                                  NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    try {
        if (feature != NVSDK_NGX_Feature_RayReconstruction) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        if (!params || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const RrCreateParams create = read_create_params(*params);
        return create_vk_rr_feature(dev, create, out);
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_create(VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                                 NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    try {
        VkDevice dev = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
            dev = g_ngx_vk_device;
        }
        return provider_vk_create1(dev, cmd, feature, params, out);
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

std::atomic<uint64_t> g_vk_skipped_frames{0};

NVSDK_NGX_Result evaluate_vk_rr(VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params) {
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::unique_lock<std::mutex> lock(g_ctx_mutex);
    auto it = g_vk_rr.find(handle->Id);
    if (it == g_vk_rr.end()) {
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    if (!cmd || !params || !g_api5.RecordVk) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    openrr::pe::watchdog::beat();

    RrContext& ctx = *it->second;
    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.mode != ctx.mode_applied) {
        ctx.mode_applied = cfg.mode;
        OnrRrSetFastMode(ctx.runtime_ctx, cfg.mode);
    }

    OnrRrFrame frame{};
    if (!fill_vk_frame(ctx, *params, frame)) return NVSDK_NGX_Result_Fail;

    int32_t rc = ONR_RR_FAILED;
    {
        InternalVkScope vk_scope;
        rc = g_api5.RecordVk(ctx.runtime_ctx, &frame, cmd);
    }
    if (rc == ONR_RR_BUSY) {
        const uint64_t skip_n = ++g_vk_skipped_frames;
        if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
            log("[openrr] Vulkan: all 64 recording slots in flight; skipping this frame (output not written, count=%llu)",
                static_cast<unsigned long long>(skip_n));
        }
        return NVSDK_NGX_Result_Success;
    }
    if (rc != ONR_RR_OK) {
        update_status_from_context(ctx);
        log("[openrr] RecordVk failed (rc=%d): %s", rc, runtime_last_error().c_str());
        return NVSDK_NGX_Result_Fail;
    }

    ++ctx.frame_count;
    if (ctx.frame_count == 1 || ctx.frame_count % 30 == 0) {
        update_status_from_context(ctx);
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle,
                                                   const NVSDK_NGX_Parameter* params, void*) {
    try { return evaluate_vk_rr(cmd, handle, const_cast<NVSDK_NGX_Parameter*>(params)); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_release(NVSDK_NGX_Handle* handle) {
    try {
        if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        std::unique_ptr<RrContext> gone;
        bool any_active = false;
        {
            std::lock_guard<std::mutex> lock(g_ctx_mutex);
            auto it = g_vk_rr.find(handle->Id);
            if (it != g_vk_rr.end()) {
                gone = std::move(it->second);
                g_vk_rr.erase(it);
            }
            any_active = !g_d3d12_rr.empty() || !g_vk_rr.empty();
            sync_watchdog_rr_active_locked();
        }
        if (!gone) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        log("[openrr] Vulkan Ray Reconstruction feature released (handle=0x%X)", gone->handle_id);
        if (gone->runtime_ctx) {
            InternalVkScope vk_scope;
            g_api.Destroy(gone->runtime_ctx);
        }
        if (gone->allocated_handle == handle) delete gone->allocated_handle;
        if (!any_active) {
            openrr::pe::control::Status st{};
            openrr::pe::control::report(st);
        }
        return NVSDK_NGX_Result_Success;
    } catch (...) {
        return NVSDK_NGX_Result_Fail;
    }
}

void destroy_all_vk_rr(VkDevice filter_dev = VK_NULL_HANDLE) {
    std::vector<std::unique_ptr<RrContext>> list;
    bool any_active = false;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        for (auto it = g_vk_rr.begin(); it != g_vk_rr.end();) {
            if (!filter_dev || (it->second && it->second->vk_device == filter_dev)) {
                list.push_back(std::move(it->second));
                it = g_vk_rr.erase(it);
            } else {
                ++it;
            }
        }
        any_active = !g_d3d12_rr.empty() || !g_vk_rr.empty();
        sync_watchdog_rr_active_locked();
    }
    for (auto& ctx : list) {
        if (!ctx) continue;
        if (ctx->vk_device) {
            openrr::pe::vulkan_entry_points();
            vkDeviceWaitIdle(ctx->vk_device);
        }
        if (ctx->runtime_ctx) {
            InternalVkScope vk_scope;
            g_api.Destroy(ctx->runtime_ctx);
        }
        if (ctx->allocated_handle) delete ctx->allocated_handle;
    }
    if (!list.empty() && !any_active) {
        openrr::pe::control::Status st{};
        openrr::pe::control::report(st);
    }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_shutdown() {
    try { destroy_all_vk_rr(); return NVSDK_NGX_Result_Success; }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

NVSDK_NGX_Result RR_NGX_MSABI provider_vk_shutdown1(VkDevice dev) {
    try { destroy_all_vk_rr(dev); return NVSDK_NGX_Result_Success; }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}

void* openrr_provider_get_proc(void*, const char* export_name) {
    if (!export_name) return nullptr;
    // D3D12 DLSSD provider exports
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_Init") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_init);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_Init_Ext") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_init_ext);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_Shutdown") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_shutdown);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_Shutdown1") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_shutdown1);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_GetScratchBufferSize") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_get_scratch);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_CreateFeature") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_create);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_release);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_get_feat_req);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_evaluate);
    if (std::strcmp(export_name, "NVSDK_NGX_D3D12_PopulateParameters_Impl") == 0)
        return reinterpret_cast<void*>(&provider_d3d12_populate_params);

    // Vulkan DLSSD provider exports
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_Init") == 0)
        return reinterpret_cast<void*>(&provider_vk_init);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_Init_Ext") == 0)
        return reinterpret_cast<void*>(&provider_vk_init_ext);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_Init_Ext2") == 0)
        return reinterpret_cast<void*>(&provider_vk_init_ext2);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_Shutdown") == 0)
        return reinterpret_cast<void*>(&provider_vk_shutdown);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_Shutdown1") == 0)
        return reinterpret_cast<void*>(&provider_vk_shutdown1);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_GetScratchBufferSize") == 0)
        return reinterpret_cast<void*>(&provider_vk_get_scratch);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_CreateFeature") == 0)
        return reinterpret_cast<void*>(&provider_vk_create);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_CreateFeature1") == 0)
        return reinterpret_cast<void*>(&provider_vk_create1);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_ReleaseFeature") == 0)
        return reinterpret_cast<void*>(&provider_vk_release);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_GetFeatureRequirements") == 0)
        return reinterpret_cast<void*>(&provider_vk_get_feat_req);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_EvaluateFeature") == 0)
        return reinterpret_cast<void*>(&provider_vk_evaluate);
    if (std::strcmp(export_name, "NVSDK_NGX_VULKAN_PopulateParameters_Impl") == 0)
        return reinterpret_cast<void*>(&provider_vk_populate_params);
    return nullptr;
}

// ---- Vulkan Device & Command-Buffer Lifetime Hooks (vkCreateDevice, vkDestroyDevice, Item #3 & #9) ----
std::mutex g_vk_dev_mutex;
std::unordered_map<VkPhysicalDevice, VkInstance> g_phys_to_inst;
std::unordered_map<VkInstance, uint32_t> g_inst_api_ver;
std::atomic<VkInstance> g_last_vk_instance{VK_NULL_HANDLE};
std::atomic<PFN_vkCreateDevice> g_next_vk_create_device{nullptr};
std::atomic<PFN_vkGetDeviceProcAddr> g_next_vk_gdpa{nullptr};

PFN_vkCreateInstance real_vk_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices real_vk_enum_phys = nullptr;
PFN_vkGetInstanceProcAddr real_vk_gipa = nullptr;
PFN_vkGetDeviceProcAddr real_vk_export_gdpa = nullptr;
PFN_vkCreateDevice real_vk_export_create_device = nullptr;
PFN_vkDestroyDevice real_vk_destroy_device = nullptr;
std::atomic<PFN_vkDestroyDevice> g_gdpa_destroy_device{nullptr};

PFN_vkAllocateCommandBuffers real_vk_export_alloc_cbs = nullptr;
PFN_vkBeginCommandBuffer real_vk_export_begin_cb = nullptr;
PFN_vkResetCommandBuffer real_vk_export_reset_cb = nullptr;
PFN_vkFreeCommandBuffers real_vk_export_free_cbs = nullptr;
PFN_vkResetCommandPool real_vk_export_reset_pool = nullptr;
PFN_vkDestroyCommandPool real_vk_export_destroy_pool = nullptr;

std::atomic<PFN_vkAllocateCommandBuffers> g_gdpa_alloc_cbs{nullptr};
std::atomic<PFN_vkBeginCommandBuffer> g_gdpa_begin_cb{nullptr};
std::atomic<PFN_vkResetCommandBuffer> g_gdpa_reset_cb{nullptr};
std::atomic<PFN_vkFreeCommandBuffers> g_gdpa_free_cbs{nullptr};
std::atomic<PFN_vkResetCommandPool> g_gdpa_reset_pool{nullptr};
std::atomic<PFN_vkDestroyCommandPool> g_gdpa_destroy_pool{nullptr};

void* g_vk_export_addrs[8]{};
thread_local int t_in_vk_create_device = 0;

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    const VkResult r = real_vk_create_instance(ci, a, out);
    if (t_in_internal_vk > 0) return r;
    if (r == VK_SUCCESS && out && *out) {
        const uint32_t ver = (ci && ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
                                 ? ci->pApplicationInfo->apiVersion
                                 : VK_API_VERSION_1_3;
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        g_inst_api_ver[*out] = ver;
        g_last_vk_instance.store(*out);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_enum_phys(VkInstance instance, uint32_t* count, VkPhysicalDevice* list) {
    const VkResult r = real_vk_enum_phys(instance, count, list);
    if (t_in_internal_vk > 0) return r;
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && instance && list && count) {
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        g_last_vk_instance.store(instance);
        for (uint32_t i = 0; i < *count; ++i) g_phys_to_inst[list[i]] = instance;
    }
    return r;
}

VkResult do_vk_create_device(PFN_vkCreateDevice create, VkInstance inst, VkPhysicalDevice phys,
                             const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    if (t_in_internal_vk > 0) return create(phys, ci, a, out);
    ensure_deferred_init();
    openrr::pe::watchdog::Phase phase("vkCreateDevice (OpenRR)");
    struct Inside { Inside() { ++t_in_vk_create_device; } ~Inside() { --t_in_vk_create_device; } } inside;

    uint32_t api_ver = VK_API_VERSION_1_3;
    {
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        auto it = g_phys_to_inst.find(phys);
        if (it != g_phys_to_inst.end()) inst = it->second;
        if (!inst) inst = g_last_vk_instance.load();
        auto vit = g_inst_api_ver.find(inst);
        if (vit != g_inst_api_ver.end()) api_ver = vit->second;
    }

    return openrr::pe::vkdevice::create_with_runtime(create, inst, api_ver, phys, ci, a, out, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                   const VkAllocationCallbacks* a, VkDevice* out) {
    return do_vk_create_device(g_next_vk_create_device.load(), g_last_vk_instance.load(), phys, ci, a, out);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                          const VkAllocationCallbacks* a, VkDevice* out) {
    if (t_in_vk_create_device || t_in_internal_vk > 0) return real_vk_export_create_device(phys, ci, a, out);
    return do_vk_create_device(real_vk_export_create_device, VK_NULL_HANDLE, phys, ci, a, out);
}

void on_vk_device_destroying(VkDevice device) {
    if (!device) return;
    destroy_all_vk_rr(device);
    OnrRrNotifyVkDeviceDestroyed(device);
    openrr::pe::vkdevice::forget_device(device);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_destroy_device(VkDevice device, const VkAllocationCallbacks* a) {
    if (t_in_internal_vk == 0) on_vk_device_destroying(device);
    real_vk_destroy_device(device, a);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_destroy_device(VkDevice device, const VkAllocationCallbacks* a) {
    if (t_in_internal_vk == 0) on_vk_device_destroying(device);
    if (auto fn = g_gdpa_destroy_device.load()) fn(device, a);
}

// Command buffer / pool reset hooks for Vulkan retirement proofs (Item #3)
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_alloc_cbs(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    return openrr::pe::vkreset::track_alloc_cbs(real_vk_export_alloc_cbs, &OnrRrNotifyVkCommandBuffersAllocated, dev, ai, out);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_alloc_cbs(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    return openrr::pe::vkreset::track_alloc_cbs(g_gdpa_alloc_cbs.load(), &OnrRrNotifyVkCommandBuffersAllocated, dev, ai, out);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    return openrr::pe::vkreset::track_begin_cb(real_vk_export_begin_cb, &OnrRrNotifyVkCommandBufferReset, cb, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    return openrr::pe::vkreset::track_begin_cb(g_gdpa_begin_cb.load(), &OnrRrNotifyVkCommandBufferReset, cb, bi);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    return openrr::pe::vkreset::track_reset_cb(real_vk_export_reset_cb, &OnrRrNotifyVkCommandBufferReset, cb, flags);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    return openrr::pe::vkreset::track_reset_cb(g_gdpa_reset_cb.load(), &OnrRrNotifyVkCommandBufferReset, cb, flags);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_export_free_cbs(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs) {
    openrr::pe::vkreset::track_free_cbs(real_vk_export_free_cbs, &OnrRrNotifyVkCommandBuffersFreed, dev, pool, count, cbs);
}
VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_free_cbs(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs) {
    openrr::pe::vkreset::track_free_cbs(g_gdpa_free_cbs.load(), &OnrRrNotifyVkCommandBuffersFreed, dev, pool, count, cbs);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_reset_pool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    return openrr::pe::vkreset::track_reset_pool(real_vk_export_reset_pool, &OnrRrNotifyVkCommandPoolReset, dev, pool, flags);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_reset_pool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    return openrr::pe::vkreset::track_reset_pool(g_gdpa_reset_pool.load(), &OnrRrNotifyVkCommandPoolReset, dev, pool, flags);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_export_destroy_pool(VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks* a) {
    openrr::pe::vkreset::track_destroy_pool(real_vk_export_destroy_pool, &OnrRrNotifyVkCommandPoolDestroyed, dev, pool, a);
}
VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_destroy_pool(VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks* a) {
    openrr::pe::vkreset::track_destroy_pool(g_gdpa_destroy_pool.load(), &OnrRrNotifyVkCommandPoolDestroyed, dev, pool, a);
}

PFN_vkVoidFunction wrap_vk_device_proc(const char* name, PFN_vkVoidFunction f);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vk_gdpa(VkDevice dev, const char* name) {
    PFN_vkGetDeviceProcAddr next = g_next_vk_gdpa.load();
    if (!next) next = real_vk_export_gdpa;
    const PFN_vkVoidFunction f = next ? next(dev, name) : nullptr;
    return wrap_vk_device_proc(name, f);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vk_export_gdpa(VkDevice dev, const char* name) {
    const PFN_vkVoidFunction f = real_vk_export_gdpa ? real_vk_export_gdpa(dev, name) : nullptr;
    return wrap_vk_device_proc(name, f);
}

PFN_vkVoidFunction wrap_vk_device_proc(const char* name, PFN_vkVoidFunction f) {
    if (!f || !name) return f;
    const void* fp = reinterpret_cast<const void*>(f);
    for (void* exp : g_vk_export_addrs) {
        if (exp && fp == exp) return f; // Already hooked via MinHook on vulkan-1.dll export
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_gdpa))
            g_next_vk_gdpa.store(reinterpret_cast<PFN_vkGetDeviceProcAddr>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_destroy_device) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_destroy_device))
            g_gdpa_destroy_device.store(reinterpret_cast<PFN_vkDestroyDevice>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_destroy_device);
    }
    if (std::strcmp(name, "vkAllocateCommandBuffers") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_alloc_cbs) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_alloc_cbs))
            g_gdpa_alloc_cbs.store(reinterpret_cast<PFN_vkAllocateCommandBuffers>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_alloc_cbs);
    }
    if (std::strcmp(name, "vkBeginCommandBuffer") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_begin_cb) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_begin_cb))
            g_gdpa_begin_cb.store(reinterpret_cast<PFN_vkBeginCommandBuffer>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_begin_cb);
    }
    if (std::strcmp(name, "vkResetCommandBuffer") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_reset_cb) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_reset_cb))
            g_gdpa_reset_cb.store(reinterpret_cast<PFN_vkResetCommandBuffer>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_reset_cb);
    }
    if (std::strcmp(name, "vkFreeCommandBuffers") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_free_cbs) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_free_cbs))
            g_gdpa_free_cbs.store(reinterpret_cast<PFN_vkFreeCommandBuffers>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_free_cbs);
    }
    if (std::strcmp(name, "vkResetCommandPool") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_reset_pool) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_reset_pool))
            g_gdpa_reset_pool.store(reinterpret_cast<PFN_vkResetCommandPool>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_reset_pool);
    }
    if (std::strcmp(name, "vkDestroyCommandPool") == 0) {
        if (f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_export_destroy_pool) && f != reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_destroy_pool))
            g_gdpa_destroy_pool.store(reinterpret_cast<PFN_vkDestroyCommandPool>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_gdpa_destroy_pool);
    }
    return f;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vk_gipa(VkInstance instance, const char* name) {
    if (instance) g_last_vk_instance.store(instance);
    const PFN_vkVoidFunction f = real_vk_gipa(instance, name);
    if (f && name && std::strcmp(name, "vkCreateDevice") == 0) {
        g_next_vk_create_device.store(reinterpret_cast<PFN_vkCreateDevice>(f));
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_vk_create_device);
    }
    return wrap_vk_device_proc(name, f);
}

std::atomic<bool> g_vk_dev_hooks_done{false};

HMODULE active_vk_module() {
    if (HMODULE wm = GetModuleHandleW(L"winevulkan.dll")) {
        if (GetProcAddress(wm, "vkCreateDevice") && GetProcAddress(wm, "vkGetInstanceProcAddr")) return wm;
    }
    return GetModuleHandleW(L"vulkan-1.dll");
}

void hook_device_creation() {
    HMODULE m = active_vk_module();
    if (!m) return;
    if (g_vk_dev_hooks_done.exchange(true, std::memory_order_acq_rel)) return;
    int exp_idx = 0;
    auto try_hk = [&](const char* name, void* detour, void** orig, bool track_export = false) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
        if (fn && MH_CreateHook(fn, detour, orig) == MH_OK) {
            MH_EnableHook(fn);
            if (track_export && exp_idx < int(std::size(g_vk_export_addrs)))
                g_vk_export_addrs[exp_idx++] = fn;
        }
    };
    try_hk("vkCreateInstance", reinterpret_cast<void*>(&hk_vk_create_instance), reinterpret_cast<void**>(&real_vk_create_instance));
    try_hk("vkEnumeratePhysicalDevices", reinterpret_cast<void*>(&hk_vk_enum_phys), reinterpret_cast<void**>(&real_vk_enum_phys));
    try_hk("vkGetInstanceProcAddr", reinterpret_cast<void*>(&hk_vk_gipa), reinterpret_cast<void**>(&real_vk_gipa));
    try_hk("vkGetDeviceProcAddr", reinterpret_cast<void*>(&hk_vk_export_gdpa), reinterpret_cast<void**>(&real_vk_export_gdpa), true);
    try_hk("vkCreateDevice", reinterpret_cast<void*>(&hk_vk_export_create_device), reinterpret_cast<void**>(&real_vk_export_create_device));
    try_hk("vkDestroyDevice", reinterpret_cast<void*>(&hk_vk_destroy_device), reinterpret_cast<void**>(&real_vk_destroy_device), true);
    try_hk("vkAllocateCommandBuffers", reinterpret_cast<void*>(&hk_vk_export_alloc_cbs), reinterpret_cast<void**>(&real_vk_export_alloc_cbs), true);
    try_hk("vkBeginCommandBuffer", reinterpret_cast<void*>(&hk_vk_export_begin_cb), reinterpret_cast<void**>(&real_vk_export_begin_cb), true);
    try_hk("vkResetCommandBuffer", reinterpret_cast<void*>(&hk_vk_export_reset_cb), reinterpret_cast<void**>(&real_vk_export_reset_cb), true);
    try_hk("vkFreeCommandBuffers", reinterpret_cast<void*>(&hk_vk_export_free_cbs), reinterpret_cast<void**>(&real_vk_export_free_cbs), true);
    try_hk("vkResetCommandPool", reinterpret_cast<void*>(&hk_vk_export_reset_pool), reinterpret_cast<void**>(&real_vk_export_reset_pool), true);
    try_hk("vkDestroyCommandPool", reinterpret_cast<void*>(&hk_vk_export_destroy_pool), reinterpret_cast<void**>(&real_vk_export_destroy_pool), true);
}

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_D3D12CreateDevice real_d3d12_create_device = nullptr;
std::atomic<bool> g_d3d12_dev_hook_done{false};

HRESULT WINAPI hk_d3d12_create_device(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid, void** out) {
    const HRESULT hr = real_d3d12_create_device(adapter, min_level, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            ensure_cmd_sig_hook(dev);
            dev->Release();
        }
    }
    return hr;
}

void hook_d3d12_device_creation() {
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    if (!m) return;
    if (g_d3d12_dev_hook_done.exchange(true, std::memory_order_acq_rel)) return;
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, "D3D12CreateDevice"));
    if (fn && MH_CreateHook(fn, reinterpret_cast<void*>(&hk_d3d12_create_device), reinterpret_cast<void**>(&real_d3d12_create_device)) == MH_OK) {
        MH_EnableHook(fn);
    }
}

// ---- NVAPI NvAPI_GPU_GetGPUInfo (0xafd1b02c) fallback ----
openrr::pe::nvapi::QueryInterfaceHooks<4> g_nvapi_hooks;

bool nvapi_can_provide() {
    if (!openrr::pe::current_config().enabled) return false;
    ensure_deferred_init();
    return check_usable(nullptr);
}

void nvapi_log_msg(const char* msg) {
    if (msg) log("[openrr] %s", msg);
}

// Late vulkan-1.dll / d3d12.dll / nvapi64.dll load notification via LdrRegisterDllNotification (Item #9)
struct UNICODE_STRING_LDR {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct LDR_DLL_LOADED_NOTIFICATION_DATA_T {
    ULONG Flags;
    const UNICODE_STRING_LDR* FullDllName;
    const UNICODE_STRING_LDR* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
union LDR_DLL_NOTIFICATION_DATA_T {
    LDR_DLL_LOADED_NOTIFICATION_DATA_T Loaded;
};
using PFN_LdrDllNotification = VOID(NTAPI*)(ULONG, const LDR_DLL_NOTIFICATION_DATA_T*, PVOID);
using PFN_LdrRegisterDllNotification = LONG(NTAPI*)(ULONG, PFN_LdrDllNotification, PVOID, PVOID*);
PVOID g_dll_notif_cookie = nullptr;

VOID NTAPI on_dll_loaded_notification(ULONG reason, const LDR_DLL_NOTIFICATION_DATA_T* data, PVOID) {
    // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED, 2 = LDR_DLL_NOTIFICATION_REASON_UNLOADED
    if ((reason != 1 && reason != 2) || !data || !data->Loaded.BaseDllName || !data->Loaded.BaseDllName->Buffer) return;
    const USHORT chars = data->Loaded.BaseDllName->Length / sizeof(wchar_t);
    if (chars == 11 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"nvapi64.dll", 11) == 0) {
        if (reason == 1) g_nvapi_hooks.hook_module(static_cast<HMODULE>(data->Loaded.DllBase));
        else g_nvapi_hooks.unhook_module(data->Loaded.DllBase, data->Loaded.SizeOfImage);
        return;
    }
    if (reason != 1) return;
    if ((chars == 12 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"vulkan-1.dll", 12) == 0) ||
        (chars == 14 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"winevulkan.dll", 14) == 0)) {
        hook_device_creation();
    } else if (chars == 9 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"d3d12.dll", 9) == 0) {
        hook_d3d12_device_creation();
    }
}

using PFN_LdrUnregisterDllNotification = LONG(NTAPI*)(PVOID);

void register_dll_notification() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg) reg(0, &on_dll_loaded_notification, nullptr, &g_dll_notif_cookie);
}

void unregister_dll_notification() {
    if (!g_dll_notif_cookie) return;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto unreg = reinterpret_cast<PFN_LdrUnregisterDllNotification>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (unreg) unreg(g_dll_notif_cookie);
    }
    g_dll_notif_cookie = nullptr;
}

// ---- Process Termination Watch ----
using PFN_NtTerminateProcess = LONG(NTAPI*)(HANDLE, LONG);
PFN_NtTerminateProcess real_terminate = nullptr;
std::atomic<bool> g_terminate_said{false}, g_exiting{false};

LONG NTAPI hk_terminate(HANDLE process, LONG status) {
    const bool self = process == GetCurrentProcess() || (process && GetProcessId(process) == GetCurrentProcessId());
    if (self && !g_exiting.load(std::memory_order_acquire)) {
        if (static_cast<uint32_t>(status) < 0xC0000000u) {
            openrr::pe::opti_plugin::flush_pending_font();
        }
        if (!g_terminate_said.exchange(true)) {
            log("[openrr] process is being ended by TerminateProcess (exit status 0x%08lX), thread %lu",
                static_cast<unsigned long>(status), GetCurrentThreadId());
        }
    }
    return real_terminate(process, status);
}

void watch_termination() {
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(ntdll, "NtTerminateProcess"));
        if (fn && MH_CreateHook(fn, reinterpret_cast<void*>(&hk_terminate), reinterpret_cast<void**>(&real_terminate)) == MH_OK)
            MH_EnableHook(fn);
    }
}

} // namespace

std::once_flag g_asi_init_once;
std::atomic<bool> g_asi_initialized{false};

void initialize_asi_from(void* caller) {
    std::call_once(g_asi_init_once, [caller] {
        HMODULE opti = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(caller), &opti);
        if (!opti || opti == g_self) opti = GetModuleHandleW(nullptr);

        openrr::pe::set_module(opti);
        openrr::pe::enable_d3d12_dred();
        openrr::pe::install_crash_watch(opti);
        std::set_terminate([] {
            log("[openrr] std::terminate: an unhandled exception terminated an OpenRR thread");
            std::abort();
        });

        log("[openrr] openrr-amd.asi v%s (%s) loaded by %s",
            ONR_RR_RUNTIME_VERSION, OPENRR_BUILD_STAMP, openrr::u8::module_file(opti).c_str());

        judge_executable();
        read_ini_barriers();
        probe_coexist_mochi();
        (void)openrr::pe::current_config();

        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
            log("[openrr] MinHook could not start (%s): OpenRR device hooks unavailable", MH_StatusToString(init));
            g_asi_initialized.store(true, std::memory_order_release);
            return;
        }
        watch_termination();
        hook_device_creation();
        hook_d3d12_device_creation();
        register_dll_notification();

        g_nvapi_hooks.set_callbacks(&nvapi_can_provide, &nvapi_log_msg);
        if (opti && GetProcAddress(opti, "nvapi_QueryInterface") && !g_nvapi_hooks.hook_module(opti)) {
            log("[openrr] nvapi_QueryInterface could not be hooked in host module");
        }
        g_nvapi_hooks.hook_module(GetModuleHandleW(L"nvapi64.dll"));

        g_asi_initialized.store(true, std::memory_order_release);
        log("[openrr] native OptiScaler plugin hooks ready (UnrealByExe=%s)", g_unreal_by_exe ? "yes" : "no");
    });
}

// Called by OptiScaler from its DllMain, under the loader lock: hooks and INI read only (Item #8).
extern "C" __declspec(dllexport) void InitializeASI() {
    try {
        initialize_asi_from(__builtin_return_address(0));
    } catch (...) {
    }
}

// Called by OptiScaler right after InitializeASI (still under loader lock for normal ASI plugins).
extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin) {
    try {
        initialize_asi_from(__builtin_return_address(0));
        return openrr::pe::opti_plugin::init(api, plugin, &openrr_provider_get_proc, &ensure_deferred_init);
    } catch (...) {
        return false;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        openrr::pe::split::set_self_module(module);
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        g_exiting = true;
        openrr::pe::opti_plugin::flush_pending_font();
        if (g_asi_initialized.load(std::memory_order_relaxed))
            log("[openrr] process is ending (ExitProcess)");
        openrr::pe::remove_crash_watch();
        for (auto& [id, sess] : g_vkd3d_sessions) if (sess) sess->abandon();
        for (auto& [id, ctx] : g_d3d12_rr) (void)ctx.release();
        for (auto& [id, ctx] : g_vk_rr) (void)ctx.release();
        openrr::pe::flush_log();
    } else if (reason == DLL_PROCESS_DETACH) {
        unregister_dll_notification();
        openrr::pe::remove_crash_watch();
        openrr::pe::flush_log();
    }
    return TRUE;
}
