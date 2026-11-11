// SPDX-License-Identifier: MIT
// openrr-amd.asi: Standalone OpenRR (DLSS Ray Reconstruction on AMD RDNA4) ASI plugin for upstream OptiScaler.
// Shell adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_path_win.hpp"
#include "rr_ngx_abi.hpp"
#include "rr_mochi_hook.hpp"
#include "rr_pe_config.hpp"
#include "rr_pe_control.hpp"
#include "rr_pe_crash.hpp"
#include "rr_pe_d3d12split.hpp"
#include "rr_pe_log.hpp"
#include "rr_pe_menu.hpp"
#include "rr_pe_vkdevice.hpp"
#include "rr_pe_watchdog.hpp"
#include "camera_and_sl.hpp"
#include "opennr_rr_api.h"

#include <windows.h>
#include <d3d11.h>
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
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef OPENRR_BUILD_STAMP
#define OPENRR_BUILD_STAMP "dev"
#endif

namespace {

using openrr::pe::log;

HMODULE g_self = nullptr;
HMODULE g_opti = nullptr;

struct Range { uintptr_t begin = 0, end = 0; };
Range g_opti_range{}, g_self_range{};

Range range_of(HMODULE m) {
    if (!m) return {};
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

bool in_range(const Range& r, const void* p) {
    const auto a = reinterpret_cast<uintptr_t>(p);
    return a >= r.begin && a < r.end;
}

template <class T> void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

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

// ---- D3D12Core MinHook compute-state tracker for Direct mode (Item #1) & Allocator Reset (Item #3) ----
bool in_d3d12_core_module(const void* fn, std::string* out_mod = nullptr) {
    if (!fn) return false;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(fn), &mod) || !mod)
        return false;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (out_mod) *out_mod = utf8_from_wide(base);
    if (_wcsnicmp(base, L"d3d12", 5) == 0) return true;
    for (const wchar_t* p = base; *p; ++p)
        if (_wcsnicmp(p, L"vkd3d", 5) == 0) return true;
    return false;
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

struct DirectListState {
    bool valid = false;
    bool unknown = false;
    UINT heap_count = 0;
    ID3D12DescriptorHeap* heaps[2]{};
    DirectRootArgs compute_root{};
    DirectRootArgs graphics_root{};
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
        graphics_root.rs = nullptr;
        graphics_root.rs_set = false;
        graphics_root.clear_bindings();
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
    } else if (info.pipeline_kind == 1) {
        apply_cmd_sig_to_root(s->graphics_root, &info);
    }
}

std::mutex g_direct_hook_install_mutex;
std::atomic<bool> g_direct_hooks_installed{false};
std::atomic<bool> g_direct_hooks_ok{false};
std::atomic<bool> g_alloc_hook_installed{false};
std::atomic<bool> g_cmd_sig_hook_installed{false};
std::atomic<bool> g_cmd_sig_hook_ok{false};
thread_local int t_in_openrr_record = 0;
thread_local ID3D12GraphicsCommandList* t_record_target = nullptr;

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
PFN_CL_SetComputeRootSignature real_cl_set_graphics_rs = nullptr;
PFN_CL_SetComputeRootDescriptorTable real_cl_set_compute_table = nullptr;
PFN_CL_SetComputeRootDescriptorTable real_cl_set_graphics_table = nullptr;
PFN_CL_SetComputeRoot32BitConstant real_cl_set_compute_const = nullptr;
PFN_CL_SetComputeRoot32BitConstant real_cl_set_graphics_const = nullptr;
PFN_CL_SetComputeRoot32BitConstants real_cl_set_compute_consts = nullptr;
PFN_CL_SetComputeRoot32BitConstants real_cl_set_graphics_consts = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_cbv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_graphics_cbv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_srv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_graphics_srv = nullptr;
PFN_CL_SetComputeRootView real_cl_set_compute_uav = nullptr;
PFN_CL_SetComputeRootView real_cl_set_graphics_uav = nullptr;
PFN_CL_ExecuteIndirect real_cl_exec_indirect = nullptr;
PFN_CL_SetPipelineState1 real_cl_set_pso1 = nullptr;
PFN_Dev_CreateCommandList real_dev_create_cl = nullptr;
PFN_Dev_CreateCommandSignature real_dev_create_cmd_sig = nullptr;
PFN_Dev4_CreateCommandList1 real_dev4_create_cl1 = nullptr;
PFN_CA_Reset real_ca_reset = nullptr;
PFN_COM_Release real_ca_release = nullptr;
PFN_COM_Release real_cl_release = nullptr;
void* g_hooked_ca_release_fn = nullptr;

HRESULT STDMETHODCALLTYPE hk_ca_reset(ID3D12CommandAllocator* alloc) {
    const HRESULT hr = real_ca_reset(alloc);
    if (SUCCEEDED(hr) && alloc) OnrRrNotifyD3d12AllocatorReset(alloc);
    return hr;
}

ULONG STDMETHODCALLTYPE hk_ca_release(void* alloc) {
    const ULONG left = real_ca_release(alloc);
    if (left == 0 && alloc) {
        OnrRrNotifyD3d12AllocatorReleased(alloc);
    }
    return left;
}

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
        direct_state_of(cl)->reset(pso);
    }
    return hr;
}

void STDMETHODCALLTYPE hk_cl_clear_state(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->clear_state(pso);
    }
    real_cl_clear_state(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        DirectListState* s = direct_state_of(cl);
        s->pso = pso;
        s->pso_set = true;
        s->so_last = false;
    }
    real_cl_set_pso(cl, pso);
}

void STDMETHODCALLTYPE hk_cl_set_pso1(ID3D12GraphicsCommandList* cl, ID3D12StateObject* so) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        DirectListState* s = direct_state_of(cl);
        s->state_object = so;
        s->so_set = true;
        s->so_last = true;
    }
    real_cl_set_pso1(cl, so);
}

void STDMETHODCALLTYPE hk_cl_exec_bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* bundle) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
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
                const DirectRootArgs& bgr = bs->graphics_root;
                if (bgr.rs_set) s->graphics_root.set_rs(bgr.rs);
                for (UINT i = 0; i < 64; ++i) {
                    if (bgr.mask & (1ull << i)) s->graphics_root.set(i, bgr.kind[i], bgr.value[i]);
                    if ((bgr.const_param_mask & (1ull << i)) && bgr.const_vals[i]) {
                        uint64_t dm = bgr.const_dword_mask[i];
                        for (UINT off = 0; off < 64; ++off)
                            if (dm & (1ull << off)) s->graphics_root.constant(i, off, (*bgr.const_vals[i])[off]);
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
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        DirectListState* s = direct_state_of(cl);
        const UINT n = std::min<UINT>(count, 2u);
        bool same = (n == s->heap_count);
        for (UINT i = 0; same && i < n; ++i) same = (s->heaps[i] == (heaps ? heaps[i] : nullptr));
        if (!same) {
            for (UINT i = 0; i < 64; ++i) {
                if ((s->compute_root.mask & (1ull << i)) && s->compute_root.kind[i] == 1)
                    s->compute_root.mask &= ~(1ull << i);
                if ((s->graphics_root.mask & (1ull << i)) && s->graphics_root.kind[i] == 1)
                    s->graphics_root.mask &= ~(1ull << i);
            }
        }
        s->heap_count = n;
        for (UINT i = 0; i < n; ++i) s->heaps[i] = heaps ? heaps[i] : nullptr;
    }
    real_cl_set_heaps(cl, count, heaps);
}

void STDMETHODCALLTYPE hk_cl_set_compute_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.set_rs(rs);
    }
    real_cl_set_compute_rs(cl, rs);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_rs(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.set_rs(rs);
    }
    real_cl_set_graphics_rs(cl, rs);
}

void STDMETHODCALLTYPE hk_cl_set_compute_table(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.set(index, 1, h.ptr);
    }
    real_cl_set_compute_table(cl, index, h);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_table(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.set(index, 1, h.ptr);
    }
    real_cl_set_graphics_table(cl, index, h);
}

void STDMETHODCALLTYPE hk_cl_set_compute_const(ID3D12GraphicsCommandList* cl, UINT index, UINT val, UINT offset) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.constant(index, offset, val);
    }
    real_cl_set_compute_const(cl, index, val, offset);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_const(ID3D12GraphicsCommandList* cl, UINT index, UINT val, UINT offset) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.constant(index, offset, val);
    }
    real_cl_set_graphics_const(cl, index, val, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_consts(ID3D12GraphicsCommandList* cl, UINT index, UINT count, const void* data, UINT offset) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl && data) {
        DirectRootArgs& r = direct_state_of(cl)->compute_root;
        const auto* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count; ++i) r.constant(index, offset + i, v[i]);
    }
    real_cl_set_compute_consts(cl, index, count, data, offset);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_consts(ID3D12GraphicsCommandList* cl, UINT index, UINT count, const void* data, UINT offset) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl && data) {
        DirectRootArgs& r = direct_state_of(cl)->graphics_root;
        const auto* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count; ++i) r.constant(index, offset + i, v[i]);
    }
    real_cl_set_graphics_consts(cl, index, count, data, offset);
}

void STDMETHODCALLTYPE hk_cl_set_compute_cbv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.set(index, 2, va);
    }
    real_cl_set_compute_cbv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_cbv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.set(index, 2, va);
    }
    real_cl_set_graphics_cbv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_srv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.set(index, 3, va);
    }
    real_cl_set_compute_srv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_srv(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.set(index, 3, va);
    }
    real_cl_set_graphics_srv(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_compute_uav(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->compute_root.set(index, 4, va);
    }
    real_cl_set_compute_uav(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_set_graphics_uav(ID3D12GraphicsCommandList* cl, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        direct_state_of(cl)->graphics_root.set(index, 4, va);
    }
    real_cl_set_graphics_uav(cl, index, va);
}

void STDMETHODCALLTYPE hk_cl_exec_indirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                                           UINT max_count, ID3D12Resource* arg_buf, UINT64 arg_offset,
                                           ID3D12Resource* count_buf, UINT64 count_offset) {
    if (t_in_openrr_record) {
        if (!t_record_target) t_record_target = cl;
    } else if (cl) {
        apply_direct_execute_indirect(direct_state_of(cl), sig);
    }
    real_cl_exec_indirect(cl, sig, max_count, arg_buf, arg_offset, count_buf, count_offset);
}

HRESULT STDMETHODCALLTYPE hk_dev_create_cmd_sig(ID3D12Device* dev, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                ID3D12RootSignature* rs, REFIID riid, void** out) {
    const HRESULT hr = real_dev_create_cmd_sig(dev, desc, rs, riid, out);
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

HRESULT STDMETHODCALLTYPE hk_dev_create_cl(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                           ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out) {
    const HRESULT hr = real_dev_create_cl(dev, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && !t_in_openrr_record) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))) && list) {
            OnrRrNotifyD3d12ListReset(list, alloc);
            direct_state_of(list)->reset(pso);
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
            direct_state_of(list)->reset(nullptr);
            list->Release();
        }
    }
    return hr;
}

bool ensure_cmd_sig_hook(ID3D12Device* dev) {
    if (g_cmd_sig_hook_installed.load(std::memory_order_acquire))
        return g_cmd_sig_hook_ok.load(std::memory_order_relaxed);
    if (!dev) return false;
    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    if (g_cmd_sig_hook_installed.load(std::memory_order_relaxed))
        return g_cmd_sig_hook_ok.load(std::memory_order_relaxed);
    void** dvt = *reinterpret_cast<void***>(dev);
    bool ok = false;
    if (in_d3d12_core_module(dvt[41])) {
        if (MH_CreateHook(dvt[41], reinterpret_cast<void*>(&hk_dev_create_cmd_sig), reinterpret_cast<void**>(&real_dev_create_cmd_sig)) == MH_OK &&
            MH_EnableHook(dvt[41]) == MH_OK) {
            ok = true;
        }
    }
    g_cmd_sig_hook_ok.store(ok, std::memory_order_relaxed);
    g_cmd_sig_hook_installed.store(true, std::memory_order_release);
    return ok;
}

void ensure_allocator_reset_hook(ID3D12Device* dev) {
    ensure_cmd_sig_hook(dev);
    if (!dev || g_alloc_hook_installed.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    if (g_alloc_hook_installed.load(std::memory_order_relaxed)) return;
    ID3D12CommandAllocator* probe = nullptr;
    if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probe))) && probe) {
        void** vt = *reinterpret_cast<void***>(probe);
        void* fn_ca_release = vt[2]; // IUnknown::Release on CommandAllocator
        void* fn_ca_reset = vt[8];   // ID3D12CommandAllocator::Reset
        std::string mod;
        if (in_d3d12_core_module(fn_ca_reset, &mod)) {
            if (MH_CreateHook(fn_ca_reset, reinterpret_cast<void*>(&hk_ca_reset), reinterpret_cast<void**>(&real_ca_reset)) == MH_OK)
                MH_EnableHook(fn_ca_reset);
        } else {
            log("[openrr] allocator Reset slot 8 is outside D3D12 module (%s); not hooking", mod.c_str());
        }
        if (in_d3d12_core_module(fn_ca_release)) {
            if (MH_CreateHook(fn_ca_release, reinterpret_cast<void*>(&hk_ca_release), reinterpret_cast<void**>(&real_ca_release)) == MH_OK &&
                MH_EnableHook(fn_ca_release) == MH_OK) {
                g_hooked_ca_release_fn = fn_ca_release;
            }
        }
        probe->Release();
    }
    g_alloc_hook_installed.store(true, std::memory_order_release);
}

bool ensure_direct_state_hooks(ID3D12Device* dev, ID3D12GraphicsCommandList* cl = nullptr) {
    ensure_allocator_reset_hook(dev);
    if (g_direct_hooks_installed.load(std::memory_order_acquire))
        return g_direct_hooks_ok.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_direct_hook_install_mutex);
    if (g_direct_hooks_installed.load(std::memory_order_relaxed))
        return g_direct_hooks_ok.load(std::memory_order_relaxed);

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
    struct HookItem { const char* name; unsigned slot; void* detour; void** orig; };
    const HookItem cl_items[] = {
        {"Reset",                             10, reinterpret_cast<void*>(&hk_cl_reset),             reinterpret_cast<void**>(&real_cl_reset)},
        {"ClearState",                        11, reinterpret_cast<void*>(&hk_cl_clear_state),       reinterpret_cast<void**>(&real_cl_clear_state)},
        {"SetPipelineState",                  25, reinterpret_cast<void*>(&hk_cl_set_pso),           reinterpret_cast<void**>(&real_cl_set_pso)},
        {"ExecuteBundle",                     27, reinterpret_cast<void*>(&hk_cl_exec_bundle),       reinterpret_cast<void**>(&real_cl_exec_bundle)},
        {"SetDescriptorHeaps",                28, reinterpret_cast<void*>(&hk_cl_set_heaps),          reinterpret_cast<void**>(&real_cl_set_heaps)},
        {"SetComputeRootSignature",           29, reinterpret_cast<void*>(&hk_cl_set_compute_rs),     reinterpret_cast<void**>(&real_cl_set_compute_rs)},
        {"SetGraphicsRootSignature",          30, reinterpret_cast<void*>(&hk_cl_set_graphics_rs),    reinterpret_cast<void**>(&real_cl_set_graphics_rs)},
        {"SetComputeRootDescriptorTable",     31, reinterpret_cast<void*>(&hk_cl_set_compute_table),  reinterpret_cast<void**>(&real_cl_set_compute_table)},
        {"SetGraphicsRootDescriptorTable",    32, reinterpret_cast<void*>(&hk_cl_set_graphics_table), reinterpret_cast<void**>(&real_cl_set_graphics_table)},
        {"SetComputeRoot32BitConstant",       33, reinterpret_cast<void*>(&hk_cl_set_compute_const),  reinterpret_cast<void**>(&real_cl_set_compute_const)},
        {"SetGraphicsRoot32BitConstant",      34, reinterpret_cast<void*>(&hk_cl_set_graphics_const), reinterpret_cast<void**>(&real_cl_set_graphics_const)},
        {"SetComputeRoot32BitConstants",      35, reinterpret_cast<void*>(&hk_cl_set_compute_consts), reinterpret_cast<void**>(&real_cl_set_compute_consts)},
        {"SetGraphicsRoot32BitConstants",     36, reinterpret_cast<void*>(&hk_cl_set_graphics_consts),reinterpret_cast<void**>(&real_cl_set_graphics_consts)},
        {"SetComputeRootConstantBufferView",  37, reinterpret_cast<void*>(&hk_cl_set_compute_cbv),    reinterpret_cast<void**>(&real_cl_set_compute_cbv)},
        {"SetGraphicsRootConstantBufferView", 38, reinterpret_cast<void*>(&hk_cl_set_graphics_cbv),   reinterpret_cast<void**>(&real_cl_set_graphics_cbv)},
        {"SetComputeRootShaderResourceView",  39, reinterpret_cast<void*>(&hk_cl_set_compute_srv),    reinterpret_cast<void**>(&real_cl_set_compute_srv)},
        {"SetGraphicsRootShaderResourceView", 40, reinterpret_cast<void*>(&hk_cl_set_graphics_srv),   reinterpret_cast<void**>(&real_cl_set_graphics_srv)},
        {"SetComputeRootUnorderedAccessView", 41, reinterpret_cast<void*>(&hk_cl_set_compute_uav),    reinterpret_cast<void**>(&real_cl_set_compute_uav)},
        {"SetGraphicsRootUnorderedAccessView",42, reinterpret_cast<void*>(&hk_cl_set_graphics_uav),   reinterpret_cast<void**>(&real_cl_set_graphics_uav)},
        {"ExecuteIndirect",                   59, reinterpret_cast<void*>(&hk_cl_exec_indirect),      reinterpret_cast<void**>(&real_cl_exec_indirect)},
    };

    std::string mod_name;
    for (const auto& it : cl_items) {
        if (!in_d3d12_core_module(vt[it.slot], &mod_name)) {
            log("[openrr] Direct state hook refused: command-list %s (slot %u) is outside D3D12 module (%s)",
                it.name, it.slot, mod_name.c_str());
            g_direct_hooks_ok.store(false, std::memory_order_relaxed);
            g_direct_hooks_installed.store(true, std::memory_order_release);
            return false;
        }
    }

    bool all_ok = true;
    for (const auto& it : cl_items) {
        if (MH_CreateHook(vt[it.slot], it.detour, it.orig) != MH_OK || MH_EnableHook(vt[it.slot]) != MH_OK) {
            log("[openrr] Direct state hook failed on %s (slot %u)", it.name, it.slot);
            all_ok = false;
        }
    }
    if (vt[2] != g_hooked_ca_release_fn && in_d3d12_core_module(vt[2])) {
        if (MH_CreateHook(vt[2], reinterpret_cast<void*>(&hk_cl_release), reinterpret_cast<void**>(&real_cl_release)) == MH_OK)
            MH_EnableHook(vt[2]);
    }

    IUnknown* cl4 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(kIID_List4, reinterpret_cast<void**>(&cl4))) && cl4) {
        void** vt4 = *reinterpret_cast<void***>(cl4);
        if (in_d3d12_core_module(vt4[75])) {
            if (MH_CreateHook(vt4[75], reinterpret_cast<void*>(&hk_cl_set_pso1), reinterpret_cast<void**>(&real_cl_set_pso1)) == MH_OK)
                MH_EnableHook(vt4[75]);
        }
        cl4->Release();
    }

    if (dev) {
        void** dvt = *reinterpret_cast<void***>(dev);
        if (in_d3d12_core_module(dvt[12])) {
            if (MH_CreateHook(dvt[12], reinterpret_cast<void*>(&hk_dev_create_cl), reinterpret_cast<void**>(&real_dev_create_cl)) == MH_OK)
                MH_EnableHook(dvt[12]);
        }
        IUnknown* dev4 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(kIID_Dev4, reinterpret_cast<void**>(&dev4))) && dev4) {
            void** dvt4 = *reinterpret_cast<void***>(dev4);
            if (in_d3d12_core_module(dvt4[51])) {
                if (MH_CreateHook(dvt4[51], reinterpret_cast<void*>(&hk_dev4_create_cl1), reinterpret_cast<void**>(&real_dev4_create_cl1)) == MH_OK)
                    MH_EnableHook(dvt4[51]);
            }
            dev4->Release();
        }
    }

    if (dev && !ensure_cmd_sig_hook(dev)) {
        log("[openrr] Direct state hook failed on CreateCommandSignature (slot 41)");
        all_ok = false;
    }

    g_direct_hooks_ok.store(all_ok, std::memory_order_relaxed);
    g_direct_hooks_installed.store(true, std::memory_order_release);
    if (all_ok) {
        log("[openrr] Direct compute-state hooks installed in %s (13 list methods + CreateCommandList + Allocator::Reset)",
            mod_name.c_str());
    }
    return all_ok;
}

void restore_direct_state(ID3D12GraphicsCommandList* cmd, ID3D12GraphicsCommandList* target) {
    if (!cmd) return;
    ID3D12GraphicsCommandList* key = target ? target : cmd;
    const DirectListState* s = direct_state_of(key);
    if (!s || !s->valid || s->unknown) return;

    ++t_in_openrr_record;
    if (s->heap_count > 0 && s->heaps[0]) {
        cmd->SetDescriptorHeaps(s->heap_count, s->heaps);
    }
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
    const DirectRootArgs& gr = s->graphics_root;
    // OpenRR never touches graphics root signature, graphics root constants, or graphics root CBV/SRV/UAVs;
    // only SetDescriptorHeaps invalidates bound graphics descriptor tables (kind == 1).
    if (s->heap_count > 0 && s->heaps[0] && gr.mask) {
        for (UINT i = 0; i < 64; ++i) {
            if ((gr.mask & (1ull << i)) && gr.kind[i] == 1) {
                cmd->SetGraphicsRootDescriptorTable(i, {gr.value[i]});
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
}

// ---- Runtime API & nvngx_dlssd.dll discovery ----
std::mutex g_rt_mutex;
OnrRrApi5 g_api5{};
OnrRrApi4& g_api4 = g_api5.v4;
OnrRrApi3& g_api3 = g_api4.v3;
OnrRrApi2& g_api2 = g_api3.v2;
OnrRrApi& g_api = g_api2.v1;
bool g_api_ready = false;

std::vector<std::wstring> g_ngx_search_paths;
std::wstring g_dlssd_path;
std::string g_dlssd_source;
bool g_usable = false;
std::string g_unusable_reason;
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

    for (const wchar_t* sub : {L"openrr-amd", L"."}) {
        const auto cand = opti_dir / sub / L"nvngx_dlssd.dll";
        if (std::filesystem::exists(cand, ec)) {
            if (out_source) *out_source = "OptiScaler folder";
            return cand.wstring();
        }
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
        static bool logged_legacy = false;
        if (!logged_legacy) {
            logged_legacy = true;
            log("[openrr] note: legacy %ls\\ directory detected and ignored; using openrr-amd\\", legacy_name);
        }
    }
    if (std::filesystem::exists(opti_dir / L"openrr-amd" / L"shaders", ec))
        return (opti_dir / L"openrr-amd").wstring();

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
    g_usable = false;
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

namespace onr {
VkPhysicalDevice pick_vk_physical_device(const std::vector<VkPhysicalDevice>& devs, const uint8_t* luid,
                                         std::string* out_name = nullptr, uint32_t* out_api_version = nullptr,
                                         bool* out_is_amd = nullptr);
} // namespace onr

namespace {

bool check_gpu_cheap_vulkan(const LUID* luid, std::string& out_gpu_name, std::string& why) {
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
    out_gpu_name = picked_name;

    if (picked_api_version < VK_API_VERSION_1_3) {
        why = picked_name + ": Vulkan API older than 1.3";
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
    if (!has_ext(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {
        why = picked_name + (picked_is_amd ? ": no cooperative matrices (needs an RDNA4 GPU)" : ": not an AMD GPU");
        return false;
    }
    if (!has_ext(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) {
        why = picked_name + (picked_is_amd ? ": not an RDNA4 GPU (RX 7000 and RX 6000 support comes later)" : ": not an AMD GPU");
        return false;
    }

    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f8};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &cm};
    vkGetPhysicalDeviceFeatures2(picked, &f2);
    if (!cm.cooperativeMatrix) {
        why = picked_name + ": no cooperative matrices (needs an RDNA4 GPU)";
        return false;
    }
    if (!f8.shaderFloat8 || !f8.shaderFloat8CooperativeMatrix) {
        why = picked_name + ": not an RDNA4 GPU (RX 7000 and RX 6000 support comes later)";
        return false;
    }
    return true;
}

struct UsabilityCheck {
    bool usable = false;
    ULONGLONG tick = 0;
    uint64_t mtime = 0;
    std::wstring path;
    std::string source, reason;
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
        g_usable = cached.usable;
        g_dlssd_path = cached.path;
        g_dlssd_source = cached.source;
        g_unusable_reason = cached.reason;
        return cached.usable;
    }
    struct SaveCheck {
        UsabilityCheck& entry;
        ULONGLONG now;
        ~SaveCheck() {
            entry = {g_usable, now, g_dlssd_path.empty() ? 0 : file_mtime_64(g_dlssd_path),
                     g_dlssd_path, g_dlssd_source, g_unusable_reason};
        }
    } save{cached, now};

    const auto t0 = std::chrono::steady_clock::now();
    const char* source = nullptr;
    g_dlssd_path = find_dlssd_locked(&source);
    g_dlssd_source = source ? source : "";

    if (g_dlssd_path.empty()) {
        g_usable = false;
        g_unusable_reason = "nvngx_dlssd.dll (310.9.1) not found";
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: %s", ms, g_unusable_reason.c_str());
        return false;
    }

    std::string why;
    if (!check_dlssd_version_310_9_1(g_dlssd_path, why)) {
        g_usable = false;
        g_unusable_reason = why;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: %s (%s)", ms, why.c_str(), utf8_from_wide(g_dlssd_path).c_str());
        return false;
    }

    std::string gpu_name;
    if (!check_gpu_cheap_vulkan(luid, gpu_name, why)) {
        g_usable = false;
        g_unusable_reason = why;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log("[openrr] cheap Check failed in %.1f ms: %s", ms, why.c_str());
        return false;
    }

    g_usable = true;
    g_unusable_reason.clear();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log("[openrr] Check passed (cheap check in %.1f ms; GPU: %s; dlssd from %s: %s)",
        ms, gpu_name.c_str(), g_dlssd_source.c_str(), utf8_from_wide(g_dlssd_path).c_str());
    return true;
}

bool query_d3d12_adapter_names(ID3D12Device* dev) {
    if (!dev) return false;
    const LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) return false;
    IDXGIAdapter1* ad = nullptr;
    if (SUCCEEDED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) && ad) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(ad->GetDesc1(&desc))) {
            openrr::pe::vkdevice::set_adapter_names(utf8_from_wide(desc.Description), "");
        }
        ad->Release();
    }
    fac->Release();
    return true;
}

// ---- Engine & Barrier config from OptiScaler.ini ----
bool g_unreal_by_exe = false;
bool g_game_colour_readable = false;
bool g_game_motion_readable = false;
std::atomic<int> g_ngx_engine{-1};
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
    char ini[MAX_PATH * 4 + 32];
    std::snprintf(ini, sizeof ini, "%s\\OptiScaler.ini", openrr::pe::module_folder());
    auto get = [&](const char* key) {
        char v[32] = {};
        openrr::u8::profile("Hotfix", key, "", v, sizeof v, ini);
        return v[0] >= '0' && v[0] <= '9' ? std::atoi(v) : -1;
    };
    g_barrier_color = get("ColorResourceBarrier");
    g_barrier_depth = get("DepthResourceBarrier");
    g_barrier_mv = get("MVResourceBarrier");
    g_barrier_output = get("OutputResourceBarrier");
}

bool is_unreal_engine() {
    return g_unreal_by_exe || g_ngx_engine.load(std::memory_order_relaxed) == int(NVSDK_NGX_ENGINE_TYPE_UNREAL);
}

// ---- Optimal settings & stats callbacks for DLSS-D (HANDOFF §3.2) ----
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

// ---- Capability parameter rewriting (HANDOFF §3.1) ----
LUID g_last_d3d12_luid{};
std::atomic<bool> g_has_d3d12_luid{false};

bool d3d12_rr_capable() {
    LUID luid{};
    const LUID* p_luid = nullptr;
    if (g_has_d3d12_luid.load(std::memory_order_acquire)) {
        luid = g_last_d3d12_luid;
        p_luid = &luid;
    }
    return check_usable(p_luid);
}

bool vk_rr_capable(VkPhysicalDevice phys = VK_NULL_HANDLE) {
    openrr::pe::vkdevice::DeviceInfo di{};
    if (!openrr::pe::vkdevice::latest_device_info(&di) || !di.prepared) {
        static bool logged_unprepared = false;
        if (!logged_unprepared) {
            logged_unprepared = true;
            log("[openrr] Vulkan capability check: game's VkDevice was not prepared at vkCreateDevice; Ray Reconstruction not offered");
        }
        return false;
    }
    if (!phys) phys = di.physical;
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

void patch_capability_params(NVSDK_NGX_Parameter* params, bool is_vk) {
    ensure_deferred_init();
    if (!params) return;
    if (!(is_vk ? vk_rr_capable() : d3d12_rr_capable())) return;

    params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, 1);
    params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, 0);
    params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, 0);
    params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, 0);
    params->Set(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, 1);

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
    params->Set("RayReconstruction.Hint.Render.Preset.DLAA", 0u);
    params->Set("RayReconstruction.Hint.Render.Preset.UltraQuality", 0u);
    params->Set("RayReconstruction.Hint.Render.Preset.Quality", 0u);
    params->Set("RayReconstruction.Hint.Render.Preset.Balanced", 0u);
    params->Set("RayReconstruction.Hint.Render.Preset.Performance", 0u);
    params->Set("RayReconstruction.Hint.Render.Preset.UltraPerformance", 0u);
}

// ---- Parameter reading helpers (HANDOFF §3.4: DLSSD.* first, then generic fallback) ----
template <class T>
bool get_param_2(const NVSDK_NGX_Parameter& p, const char* k1, const char* k2, T& out) {
    if (k1 && p.Get(k1, &out) == NVSDK_NGX_Result_Success) return true;
    if (k2 && p.Get(k2, &out) == NVSDK_NGX_Result_Success) return true;
    return false;
}

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
    unsigned int w = 0, h = 0, sub_w = 0, sub_h = 0, out_w = 0, out_h = 0;
    p->Get(NVSDK_NGX_Parameter_Width, &w);
    p->Get(NVSDK_NGX_Parameter_Height, &h);
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &sub_w);
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &sub_h);
    p->Get(NVSDK_NGX_Parameter_OutWidth, &out_w);
    p->Get(NVSDK_NGX_Parameter_OutHeight, &out_h);

    if (w > 16384 || w < 20) w = 0;
    if (h > 16384 || h < 20) h = 0;
    if (out_w > 16384 || out_w < 20) out_w = 0;
    if (out_h > 16384 || out_h < 20) out_h = 0;

    if (w && h && out_w && out_h) {
        ow = std::max(w, out_w);
        oh = std::max(h, out_h);
        rw = sub_w ? sub_w : std::min(w, out_w);
        rh = sub_h ? sub_h : std::min(h, out_h);
    } else {
        rw = sub_w ? sub_w : (w ? w : out_w);
        rh = sub_h ? sub_h : (h ? h : out_h);
        ow = out_w ? out_w : rw;
        oh = out_h ? out_h : rh;
    }
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
    std::string mode_applied;
    uint64_t frame_count = 0;
    uint32_t unknown_streak = 0;
    ULONGLONG last_eval_tick = 0;
    OnrRrVkImage vk_images[24]{};
    int num_vk_images = 0;
};

std::mutex g_ctx_mutex;
std::unordered_map<unsigned int, std::unique_ptr<RrContext>> g_d3d12_rr;
std::unordered_map<unsigned int, std::unique_ptr<RrContext>> g_vk_rr;
bool g_ngx_depth_seen = false;
bool g_ngx_depth_hw = true;
bool g_ngx_roughness_seen = false;
int g_ngx_roughness_packed = -1;

bool fill_d3d12_frame(RrContext& ctx, const NVSDK_NGX_Parameter& params, OnrRrFrame& frame, bool host_owned) {
    const openrr::pe::Config cfg = openrr::pe::current_config();
    frame = {};
    frame.size = sizeof(frame);

    read_eval_render_size(&params, ctx.max_render_w, ctx.max_render_h, frame.render_width, frame.render_height);

    get_param_2(params, "DLSSD.Jitter.Offset.X", NVSDK_NGX_Parameter_Jitter_Offset_X, frame.jitter_x);
    get_param_2(params, "DLSSD.Jitter.Offset.Y", NVSDK_NGX_Parameter_Jitter_Offset_Y, frame.jitter_y);
    if (!std::isfinite(frame.jitter_x)) frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y)) frame.jitter_y = 0.0f;

    if (!get_param_2(params, "DLSSD.MV.Scale.X", NVSDK_NGX_Parameter_MV_Scale_X, frame.mv_scale_x) ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (!get_param_2(params, "DLSSD.MV.Scale.Y", NVSDK_NGX_Parameter_MV_Scale_Y, frame.mv_scale_y) ||
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

    auto tex = [&](const char* k1, const char* k2, const char* bx1, const char* bx2, const char* by1, const char* by2, uint32_t st) {
        OnrRrTexture t{};
        if (ID3D12Resource* r = get_d3d12_res(params, k1, k2)) {
            t.resource = r;
            t.state = st;
            get_param_2(params, bx1, bx2, t.x);
            get_param_2(params, by1, by2, t.y);
        }
        return t;
    };

    frame.color = tex(NVSDK_NGX_Parameter_Color, "DLSSD.Color",
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, "DLSSD.Input.Color.Subrect.Base.X",
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, "DLSSD.Input.Color.Subrect.Base.Y", color_state);
    frame.diffuse_albedo = tex("DLSSD.DiffuseAlbedo", NVSDK_NGX_Parameter_DiffuseAlbedo,
                               "DLSSD.Input.DiffuseAlbedo.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                               "DLSSD.Input.DiffuseAlbedo.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read_state);
    if (!frame.diffuse_albedo.resource)
        frame.diffuse_albedo = tex("GBuffer.DiffuseAlbedo", "GBuffer.Albedo", nullptr, nullptr, nullptr, nullptr, read_state);

    frame.specular_albedo = tex("DLSSD.SpecularAlbedo", NVSDK_NGX_Parameter_SpecularAlbedo,
                                "DLSSD.Input.SpecularAlbedo.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                                "DLSSD.Input.SpecularAlbedo.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read_state);
    if (!frame.specular_albedo.resource)
        frame.specular_albedo = tex("GBuffer.SpecularAlbedo", "GBuffer.Specular", nullptr, nullptr, nullptr, nullptr, read_state);

    frame.normals = tex("DLSSD.Normals", NVSDK_NGX_Parameter_GBuffer_Normals,
                        "DLSSD.Input.Normals.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                        "DLSSD.Input.Normals.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read_state);
    frame.roughness = tex("DLSSD.Roughness", NVSDK_NGX_Parameter_GBuffer_Roughness,
                          "DLSSD.Input.Roughness.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                          "DLSSD.Input.Roughness.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read_state);
    frame.depth = tex("DLSSD.Depth", NVSDK_NGX_Parameter_Depth,
                      "DLSSD.Input.Depth.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                      "DLSSD.Input.Depth.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depth_state);
    frame.motion = tex("DLSSD.MotionVectors", NVSDK_NGX_Parameter_MotionVectors,
                       "DLSSD.Input.MV.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                       "DLSSD.Input.MV.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, mv_state);
    frame.specular_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, "RayTracingHitDistance",
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X, nullptr,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, nullptr, read_state);
    frame.specular_ray_direction_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance, nullptr,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X, nullptr,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, nullptr, read_state);
    // Preserve ordinary output priority; only the host prefers generic Output.
    frame.output = tex(host_owned ? NVSDK_NGX_Parameter_Output : "DLSSD.Output",
                       host_owned ? "DLSSD.Output" : NVSDK_NGX_Parameter_Output,
                       "DLSS.Output.Subrect.Base.X", nullptr,
                       "DLSS.Output.Subrect.Base.Y", nullptr, output_state);

    frame.transparency_layer = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, "DLSS.TransparencyLayer",
                                   "DLSS.TransparencyLayer.Subrect.Base.X", nullptr,
                                   "DLSS.TransparencyLayer.Subrect.Base.Y", nullptr, read_state);
    frame.transparency_opacity = tex("DLSS.TransparencyLayerOpacity", nullptr,
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.X", nullptr,
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", nullptr, read_state);
    frame.disocclusion_mask = tex("DLSS.DisocclusionMask", "GBuffer.DisocclusionMask",
                                  "DLSS.DisocclusionMask.Subrect.Base.X", nullptr,
                                  "DLSS.DisocclusionMask.Subrect.Base.Y", nullptr, read_state);
    frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide,
                          nullptr, nullptr, nullptr, nullptr, read_state);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;

    frame.responsivity_mask = tex("DLSSD.ResponsivityMask", nullptr,
                                  "DLSSD.ResponsivityMask.Subrect.Base.X", nullptr,
                                  "DLSSD.ResponsivityMask.Subrect.Base.Y", nullptr, read_state);
    frame.color_before_transparency = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, read_state);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;

    frame.depth_of_field_guide = tex(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr,
                                     nullptr, nullptr, nullptr, nullptr, read_state);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;

    frame.specular_motion = tex(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, "MotionVectorsReflection",
                                nullptr, nullptr, nullptr, nullptr, read_state);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;

    frame.transparency_motion = tex("DLSS.TransparencyLayerMvecs", nullptr,
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.X", nullptr,
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", nullptr, read_state);

    frame.disocclusion_mask_scale = cfg.disocclusion_mask_scale > 0.0f ? cfg.disocclusion_mask_scale : 1.0f;
    if (!cfg.use_disocclusion_mask) frame.disocclusion_mask = {};
    if (!cfg.use_sss_guide) frame.sss_guide = {};
    if (!cfg.use_responsivity_mask) frame.responsivity_mask = {};
    if (!cfg.use_color_before_transparency) frame.color_before_transparency = {};
    if (!cfg.use_depth_of_field_guide) frame.depth_of_field_guide = {};
    if (!cfg.use_reflection_motion_vectors) frame.specular_motion = {};
    if (!cfg.use_layer_opacity_and_motion) frame.transparency_opacity = frame.transparency_motion = {};

    if (!frame.color.resource || !frame.depth.resource || !frame.motion.resource || !frame.output.resource) {
        log("[openrr] D3D12 evaluate missing required resource (color=%p depth=%p motion=%p output=%p)",
            frame.color.resource, frame.depth.resource, frame.motion.resource, frame.output.resource);
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

    bool hw_depth = true;
    const DepthKind dkind = classify_d3d12_depth(static_cast<ID3D12Resource*>(frame.depth.resource));
    if (cfg.hardware_depth.has_value()) hw_depth = cfg.hardware_depth.value();
    else if (dkind == DepthKind::Hardware) hw_depth = true;
    else if (ctx.has_ngx_depth_type) hw_depth = ctx.ngx_hw_depth;
    else hw_depth = (dkind != DepthKind::GameWritten);

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

    get_param_2(params, "DLSSD.Jitter.Offset.X", NVSDK_NGX_Parameter_Jitter_Offset_X, frame.jitter_x);
    get_param_2(params, "DLSSD.Jitter.Offset.Y", NVSDK_NGX_Parameter_Jitter_Offset_Y, frame.jitter_y);
    if (!std::isfinite(frame.jitter_x)) frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y)) frame.jitter_y = 0.0f;

    if (!get_param_2(params, "DLSSD.MV.Scale.X", NVSDK_NGX_Parameter_MV_Scale_X, frame.mv_scale_x) ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (!get_param_2(params, "DLSSD.MV.Scale.Y", NVSDK_NGX_Parameter_MV_Scale_Y, frame.mv_scale_y) ||
        !std::isfinite(frame.mv_scale_y) || frame.mv_scale_y == 0.0f)
        frame.mv_scale_y = 1.0f;

    const bool depth_inverted = (ctx.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    openrr::camera::ResolveCameraMatrices(params, depth_inverted, frame.world_to_view, frame.view_to_clip);

    const uint32_t read_layout = (cfg.vk_input_layout == "general")
                                     ? VK_IMAGE_LAYOUT_GENERAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const uint32_t output_layout = VK_IMAGE_LAYOUT_GENERAL;

    auto tex = [&](const char* k1, const char* k2, const char* bx1, const char* bx2, const char* by1, const char* by2, uint32_t layout) {
        OnrRrTexture t{};
        NVSDK_NGX_Resource_VK* res = get_vk_res(params, k1, k2);
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
        get_param_2(params, bx1, bx2, t.x);
        get_param_2(params, by1, by2, t.y);
        return t;
    };

    frame.color = tex(NVSDK_NGX_Parameter_Color, "DLSSD.Color",
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, "DLSSD.Input.Color.Subrect.Base.X",
                      NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, "DLSSD.Input.Color.Subrect.Base.Y", read_layout);
    frame.diffuse_albedo = tex("DLSSD.DiffuseAlbedo", NVSDK_NGX_Parameter_DiffuseAlbedo,
                               "DLSSD.Input.DiffuseAlbedo.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                               "DLSSD.Input.DiffuseAlbedo.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read_layout);
    if (!frame.diffuse_albedo.resource)
        frame.diffuse_albedo = tex("GBuffer.DiffuseAlbedo", "GBuffer.Albedo", nullptr, nullptr, nullptr, nullptr, read_layout);

    frame.specular_albedo = tex("DLSSD.SpecularAlbedo", NVSDK_NGX_Parameter_SpecularAlbedo,
                                "DLSSD.Input.SpecularAlbedo.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                                "DLSSD.Input.SpecularAlbedo.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read_layout);
    if (!frame.specular_albedo.resource)
        frame.specular_albedo = tex("GBuffer.SpecularAlbedo", "GBuffer.Specular", nullptr, nullptr, nullptr, nullptr, read_layout);

    frame.normals = tex("DLSSD.Normals", NVSDK_NGX_Parameter_GBuffer_Normals,
                        "DLSSD.Input.Normals.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                        "DLSSD.Input.Normals.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read_layout);
    frame.roughness = tex("DLSSD.Roughness", NVSDK_NGX_Parameter_GBuffer_Roughness,
                          "DLSSD.Input.Roughness.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                          "DLSSD.Input.Roughness.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read_layout);
    frame.depth = tex("DLSSD.Depth", NVSDK_NGX_Parameter_Depth,
                      "DLSSD.Input.Depth.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                      "DLSSD.Input.Depth.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, read_layout);
    frame.motion = tex("DLSSD.MotionVectors", NVSDK_NGX_Parameter_MotionVectors,
                       "DLSSD.Input.MV.Subrect.Base.X", NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                       "DLSSD.Input.MV.Subrect.Base.Y", NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, read_layout);
    frame.specular_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, "RayTracingHitDistance",
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X, nullptr,
                                      NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, nullptr, read_layout);
    frame.specular_ray_direction_hit_distance = tex(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance, nullptr,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X, nullptr,
                                                    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, nullptr, read_layout);
    frame.output = tex("DLSSD.Output", NVSDK_NGX_Parameter_Output,
                       "DLSS.Output.Subrect.Base.X", nullptr,
                       "DLSS.Output.Subrect.Base.Y", nullptr, output_layout);

    frame.transparency_layer = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, "DLSS.TransparencyLayer",
                                   "DLSS.TransparencyLayer.Subrect.Base.X", nullptr,
                                   "DLSS.TransparencyLayer.Subrect.Base.Y", nullptr, read_layout);
    frame.transparency_opacity = tex("DLSS.TransparencyLayerOpacity", nullptr,
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.X", nullptr,
                                     "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", nullptr, read_layout);
    frame.disocclusion_mask = tex("DLSS.DisocclusionMask", "GBuffer.DisocclusionMask",
                                  "DLSS.DisocclusionMask.Subrect.Base.X", nullptr,
                                  "DLSS.DisocclusionMask.Subrect.Base.Y", nullptr, read_layout);
    frame.sss_guide = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide,
                          nullptr, nullptr, nullptr, nullptr, read_layout);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;

    frame.responsivity_mask = tex("DLSSD.ResponsivityMask", nullptr,
                                  "DLSSD.ResponsivityMask.Subrect.Base.X", nullptr,
                                  "DLSSD.ResponsivityMask.Subrect.Base.Y", nullptr, read_layout);
    frame.color_before_transparency = tex(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, read_layout);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;

    frame.depth_of_field_guide = tex(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr,
                                     nullptr, nullptr, nullptr, nullptr, read_layout);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;

    frame.specular_motion = tex(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, "MotionVectorsReflection",
                                nullptr, nullptr, nullptr, nullptr, read_layout);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;

    frame.transparency_motion = tex("DLSS.TransparencyLayerMvecs", nullptr,
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.X", nullptr,
                                    "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", nullptr, read_layout);

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

void update_status_from_context(const RrContext& ctx, bool split_used) {
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
    st.split_active = split_used;
    st.weights_source = info.weights_source;
    st.message = info.message;
    st.backend_message = info.backend_message;
    openrr::pe::control::report(st);
}

// ---- NGX Hook Function Signatures & Detours ----
using PFN_D3D12_Init = NVSDK_NGX_Result(RR_NGX_MSABI*)(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_D3D12_Init_Ext = NVSDK_NGX_Result(RR_NGX_MSABI*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_D3D12_Init_ProjectID = NVSDK_NGX_Result(RR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_D3D12_Init_with_ProjectID = NVSDK_NGX_Result(RR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_GetParams = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Parameter**);
using PFN_PopulateParams = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Parameter*);
using PFN_D3D12_GetFeatureReq = NVSDK_NGX_Result(RR_NGX_MSABI*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
using PFN_GetScratchSize = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
using PFN_D3D12_CreateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_D3D12_EvaluateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
using PFN_ReleaseFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(NVSDK_NGX_Handle*);
using PFN_Shutdown = NVSDK_NGX_Result(RR_NGX_MSABI*)(void);
using PFN_D3D12_Shutdown1 = NVSDK_NGX_Result(RR_NGX_MSABI*)(ID3D12Device*);

PFN_D3D12_Init real_d3d12_init = nullptr;
PFN_D3D12_Init_Ext real_d3d12_init_ext = nullptr;
PFN_D3D12_Init_ProjectID real_d3d12_init_project = nullptr;
PFN_D3D12_Init_with_ProjectID real_d3d12_init_with_project = nullptr;
PFN_GetParams real_d3d12_get_cap_params = nullptr;
PFN_GetParams real_d3d12_get_params = nullptr;
PFN_GetParams real_d3d12_alloc_params = nullptr;
PFN_PopulateParams real_d3d12_populate_params = nullptr;
PFN_D3D12_GetFeatureReq real_d3d12_get_feat_req = nullptr;
PFN_GetScratchSize real_d3d12_get_scratch = nullptr;
PFN_D3D12_CreateFeature real_d3d12_create = nullptr;
PFN_D3D12_EvaluateFeature real_d3d12_evaluate = nullptr;
PFN_ReleaseFeature real_d3d12_release = nullptr;
PFN_Shutdown real_d3d12_shutdown = nullptr;
PFN_D3D12_Shutdown1 real_d3d12_shutdown1 = nullptr;

void on_dxgi_present_cb();

void on_d3d12_device_seen(ID3D12Device* dev) {
    ensure_cmd_sig_hook(dev);
    ensure_deferred_init();
    if (!dev) return;
    g_last_d3d12_luid = dev->GetAdapterLuid();
    g_has_d3d12_luid.store(true, std::memory_order_release);
    query_d3d12_adapter_names(dev);
    if (check_usable(&g_last_d3d12_luid)) {
        const openrr::pe::Config cfg = openrr::pe::current_config();
        if (cfg.backend == "vulkan" && split_allowed_by_config()) {
            ensure_allocator_reset_hook(dev);
        } else {
            ensure_direct_state_hooks(dev, nullptr);
        }
    }
    openrr::pe::menu::on_present_beat(&on_dxgi_present_cb);
    openrr::pe::menu::prepare_dxgi();
    openrr::pe::menu::install();
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_init(unsigned long long app, const wchar_t* path, ID3D12Device* dev,
                                            const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version ver) {
    note_feature_common_info(info);
    on_d3d12_device_seen(dev);
    return real_d3d12_init(app, path, dev, info, ver);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_init_ext(unsigned long long app, const wchar_t* path, ID3D12Device* dev,
                                                NVSDK_NGX_Version ver, const NVSDK_NGX_FeatureCommonInfo* info) {
    note_feature_common_info(info);
    on_d3d12_device_seen(dev);
    return real_d3d12_init_ext(app, path, dev, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_init_project(const char* proj, int eng, const char* ver_str, const wchar_t* path,
                                                    ID3D12Device* dev, NVSDK_NGX_Version ver,
                                                    const NVSDK_NGX_FeatureCommonInfo* info) {
    g_ngx_engine.store(eng);
    note_feature_common_info(info);
    on_d3d12_device_seen(dev);
    return real_d3d12_init_project(proj, eng, ver_str, path, dev, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_init_with_project(const char* proj, int eng, const char* ver_str, const wchar_t* path,
                                                         ID3D12Device* dev, const NVSDK_NGX_FeatureCommonInfo* info,
                                                         NVSDK_NGX_Version ver) {
    g_ngx_engine.store(eng);
    note_feature_common_info(info);
    on_d3d12_device_seen(dev);
    return real_d3d12_init_with_project(proj, eng, ver_str, path, dev, info, ver);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_get_cap_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_d3d12_get_cap_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, false);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_get_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_d3d12_get_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, false);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_alloc_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_d3d12_alloc_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, false);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_populate_params(NVSDK_NGX_Parameter* params) {
    const NVSDK_NGX_Result r = real_d3d12_populate_params(params);
    if (r == NVSDK_NGX_Result_Success && params) patch_capability_params(params, false);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_get_feat_req(IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                                    NVSDK_NGX_FeatureRequirement* out) {
    ensure_deferred_init();
    if (info) note_feature_common_info(info->FeatureInfo);
    const NVSDK_NGX_Result r = real_d3d12_get_feat_req ? real_d3d12_get_feat_req(adapter, info, out)
                                                       : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (info && info->FeatureID == NVSDK_NGX_Feature_RayReconstruction && out) {
        LUID luid{};
        const LUID* p_luid = nullptr;
        if (adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                luid = desc.AdapterLuid;
                p_luid = &luid;
            }
        }
        if ((p_luid ? check_usable(p_luid) : d3d12_rr_capable())) {
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
            out->MinHWArchitecture = 0;
            std::snprintf(out->MinOSVersion, sizeof(out->MinOSVersion), "10.0.10240.16384");
            return NVSDK_NGX_Result_Success;
        }
    }
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_get_scratch(NVSDK_NGX_Feature feat, const NVSDK_NGX_Parameter* params, size_t* out) {
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (feat == NVSDK_NGX_Feature_RayReconstruction && d3d12_rr_capable()) {
        *out = 0;
        return NVSDK_NGX_Result_Success;
    }
    return real_d3d12_get_scratch ? real_d3d12_get_scratch(feat, params, out) : NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result create_d3d12_rr(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                               NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out, bool host_owned) {
    // An inner host call must never recurse into the outer OptiScaler entry.
    const auto fallback = [&]() {
        return host_owned ? NVSDK_NGX_Result_FAIL_UnableToInitializeFeature
                          : real_d3d12_create(cmd, feature, params, out);
    };
    if (feature != NVSDK_NGX_Feature_RayReconstruction) {
        return host_owned ? NVSDK_NGX_Result_FAIL_FeatureNotSupported : fallback();
    }
    ensure_deferred_init();
    if (!cmd || !params || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (!cfg.enabled) {
        return fallback();
    }

    ID3D12Device* dev = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
        return fallback();
    }
    struct DevHold { ID3D12Device* d; ~DevHold() { if (d) d->Release(); } } hold{dev};
    on_d3d12_device_seen(dev);

    const LUID luid = dev->GetAdapterLuid();
    if (!check_usable(&luid)) {
        return fallback();
    }

    // Re-probe mochi coexistence at Create time in case dlssnr-amd-loadlate.asi finished loading (Item #7).
    const bool mochi = probe_coexist_mochi();

    // Only install command-list split hooks when the Vulkan interop bridge is requested;
    // D3D12 Direct records straight into the game's list without splitting (HANDOFF §3.6, Item #1).
    if (cfg.backend == "vulkan" && split_allowed_by_config()) {
        ensure_allocator_reset_hook(dev);
        std::string why;
        openrr::pe::split::prepare(cmd, &why);
    } else {
        ensure_direct_state_hooks(dev, cmd);
    }

    auto ctx = std::make_unique<RrContext>();
    ctx->is_vk = false;
    ctx->d3d12_device = dev;

    read_create_sizes(params, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h);
    if (!ctx->max_render_w || !ctx->max_render_h) {
        log("[openrr] D3D12 CreateFeature: invalid dimensions (%ux%u)", ctx->max_render_w, ctx->max_render_h);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

    int flags = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success)
        ctx->create_flags = uint32_t(flags);

    int depth_val = 0;
    if (params->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth_val) == NVSDK_NGX_Result_Success) {
        ctx->has_ngx_depth_type = true;
        ctx->ngx_hw_depth = (depth_val == NVSDK_NGX_DLSS_Depth_Type_HW);
        g_ngx_depth_seen = true;
        g_ngx_depth_hw = ctx->ngx_hw_depth;
    } else if (g_ngx_depth_seen) {
        ctx->has_ngx_depth_type = true;
        ctx->ngx_hw_depth = g_ngx_depth_hw;
    }

    int rough_val = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &rough_val) == NVSDK_NGX_Result_Success) {
        ctx->roughness_packed = (rough_val == NVSDK_NGX_DLSS_Roughness_Mode_Packed) ? 1 : 0;
        g_ngx_roughness_seen = true;
        g_ngx_roughness_packed = ctx->roughness_packed;
    } else if (g_ngx_roughness_seen) {
        ctx->roughness_packed = g_ngx_roughness_packed;
    }

    const std::wstring data_dir = resolve_data_dir();
    const std::wstring log_path = resolve_log_path();
    std::wstring dlssd;
    {
        std::lock_guard<std::mutex> lock(g_rt_mutex);
        dlssd = g_dlssd_path;
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
    ci.d3d12_device = dev;
    ci.max_render_width = ctx->max_render_w;
    ci.max_render_height = ctx->max_render_h;
    ci.output_width = ctx->output_w;
    ci.output_height = ctx->output_h;
    ci.dlssd_path = dlssd.c_str();
    ci.data_dir = data_dir.c_str();
    ci.log_path = log_path.c_str();
    ci.backend = backend_choice;

    openrr::pe::vulkan_entry_points();
    const int32_t rc = g_api.Create(&ci, &ctx->runtime_ctx);
    if (rc != ONR_RR_OK || !ctx->runtime_ctx) {
        const std::string err = runtime_last_error();
        log("[openrr] D3D12 Create failed (rc=%d): %s", rc, err.c_str());
        block_session("Create failed: " + err);
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    ctx->holder->ctx = ctx->runtime_ctx;

    ctx->mode_applied = cfg.mode;
    OnrRrSetFastMode(ctx->runtime_ctx, cfg.mode == "new" ? 1 : 0);

    const unsigned int id = g_next_rr_handle.fetch_add(1);
    ctx->handle_id = id;
    if (*out == nullptr) {
        ctx->allocated_handle = new NVSDK_NGX_Handle{id};
        *out = ctx->allocated_handle;
    } else {
        (*out)->Id = id;
    }

    uint32_t running_be = ONR_RR_BACKEND_D3D12;
    if (g_api4.GetBackend) g_api4.GetBackend(ctx->runtime_ctx, &running_be);
    if (running_be == ONR_RR_BACKEND_VULKAN && split_allowed_by_config()) {
        std::string why;
        openrr::pe::split::prepare(cmd, &why);
    }

    log("[openrr] D3D12 Ray Reconstruction feature created (handle=0x%X, render=%ux%u -> output=%ux%u, backend=%s, coexist_mochi=%s)",
        id, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h,
        running_be == ONR_RR_BACKEND_D3D12 ? "D3D12 Direct" : "Vulkan Bridge",
        mochi ? "yes" : "no");

    update_status_from_context(*ctx, running_be == ONR_RR_BACKEND_VULKAN);
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_d3d12_rr[id] = std::move(ctx);
    }
    return NVSDK_NGX_Result_Success;
}

std::atomic<uint64_t> g_d3d_skipped_frames{0};

#ifdef OPENRR_EXPERIMENTAL_OPTIFG_HOST
bool prepare_optifg_host();
bool g_optifg_host_active = false;
#endif
NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_create(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                                              NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
#ifdef OPENRR_EXPERIMENTAL_OPTIFG_HOST
    if (feature == NVSDK_NGX_Feature_RayReconstruction && prepare_optifg_host())
        return real_d3d12_create(cmd, feature, params, out);
#endif
    return create_d3d12_rr(cmd, feature, params, out, false);
}

NVSDK_NGX_Result evaluate_d3d12_rr(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
                                 NVSDK_NGX_Parameter* params, void* callback, bool host_owned) {
    if (!handle) return host_owned ? NVSDK_NGX_Result_FAIL_InvalidParameter : real_d3d12_evaluate(cmd, handle, params, callback);

    std::unique_lock<std::mutex> lock(g_ctx_mutex);
    auto it = g_d3d12_rr.find(handle->Id);
    if (it == g_d3d12_rr.end()) {
        lock.unlock();
        return host_owned ? NVSDK_NGX_Result_FAIL_FeatureNotFound : real_d3d12_evaluate(cmd, handle, params, callback);
    }
    if (!cmd || !params) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    RrContext& ctx = *it->second;
    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.mode != ctx.mode_applied) {
        ctx.mode_applied = cfg.mode;
        OnrRrSetFastMode(ctx.runtime_ctx, cfg.mode == "new" ? 1 : 0);
    }

    OnrRrFrame frame{};
    if (!fill_d3d12_frame(ctx, *params, frame, host_owned)) {
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
            return host_owned ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
        }
        static bool logged_fallback = false;
        if (!logged_fallback) {
            logged_fallback = true;
            log("[openrr] Vulkan bridge cannot split command list (%s)",
                can_split ? split_why.c_str() : "disabled by coexistence rule");
        }
        return NVSDK_NGX_Result_Fail;
    }

    bool used_split = false;
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
                    return host_owned ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
                }
                if (streak == 3) {
                    log("[openrr] D3D12 Direct: command-list state remained untracked for %u frames; failing evaluation", streak);
                }
                return NVSDK_NGX_Result_Fail;
            }
            ctx.unknown_streak = 0;
        }
        const auto state = split_ok ? openrr::pe::split::snapshot(cmd) : nullptr;
        void* job = nullptr;
        t_record_target = nullptr;
        ++t_in_openrr_record;
        const int32_t rc = g_api4.Record(ctx.runtime_ctx, &frame, cmd, ONR_RR_EDIT_OFF, nullptr, &job);
        --t_in_openrr_record;
        ID3D12GraphicsCommandList* target = t_record_target;
        t_record_target = nullptr;
        if (split_ok && state) {
            openrr::pe::split::restore(cmd, state);
        } else {
            restore_direct_state(cmd, target);
        }
        if (rc == ONR_RR_BUSY) {
            const uint64_t skip_n = ++g_d3d_skipped_frames;
            if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
                log("[openrr] D3D12 Direct: all 127 recording slots in flight; skipping this frame (output not written, count=%llu)",
                    static_cast<unsigned long long>(skip_n));
            }
            return host_owned ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
        }
        if (rc != ONR_RR_OK) {
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
            log("[openrr] D3D12 RecordOutput failed (rc=%d): %s", rc_out, runtime_last_error().c_str());
            return NVSDK_NGX_Result_Fail;
        }
        out_ok->store(true, std::memory_order_release);
        used_split = true;
    }

    ++ctx.frame_count;
    if (ctx.frame_count <= 32 || ctx.frame_count % 30 == 0) {
        update_status_from_context(ctx, used_split);
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
    if (rt) g_api.Destroy(rt);
}

NVSDK_NGX_Result release_d3d12_rr(NVSDK_NGX_Handle* handle, bool host_owned) {
    if (!handle) return host_owned ? NVSDK_NGX_Result_FAIL_InvalidParameter : real_d3d12_release(handle);
    std::unique_ptr<RrContext> gone;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        auto it = g_d3d12_rr.find(handle->Id);
        if (it != g_d3d12_rr.end()) {
            gone = std::move(it->second);
            g_d3d12_rr.erase(it);
        }
    }
    if (!gone) {
        return host_owned ? NVSDK_NGX_Result_FAIL_FeatureNotFound : real_d3d12_release(handle);
    }
    log("[openrr] D3D12 Ray Reconstruction feature released (handle=0x%X)", gone->handle_id);
    destroy_d3d12_rr_context(*gone);
    if (gone->allocated_handle == handle) delete gone->allocated_handle;
    openrr::pe::control::Status st{};
    openrr::pe::control::report(st);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
                                                NVSDK_NGX_Parameter* params, void* callback) {
    return evaluate_d3d12_rr(cmd, handle, params, callback, false);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_release(NVSDK_NGX_Handle* handle) {
    return release_d3d12_rr(handle, false);
}

void destroy_all_d3d12_rr() {
    std::unordered_map<unsigned int, std::unique_ptr<RrContext>> all;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        all.swap(g_d3d12_rr);
    }
    for (auto& [id, ctx] : all) {
        if (ctx) {
            destroy_d3d12_rr_context(*ctx);
            if (ctx->allocated_handle) delete ctx->allocated_handle;
        }
    }
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_shutdown() {
    destroy_all_d3d12_rr();
    return real_d3d12_shutdown ? real_d3d12_shutdown() : NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_d3d12_shutdown1(ID3D12Device* dev) {
    destroy_all_d3d12_rr();
    return real_d3d12_shutdown1 ? real_d3d12_shutdown1(dev) : NVSDK_NGX_Result_Success;
}

// ---- Vulkan NGX Hooks & Device Tracking ----
using PFN_VK_Init = NVSDK_NGX_Result(RR_NGX_MSABI*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_VK_Init_Ext = NVSDK_NGX_Result(RR_NGX_MSABI*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_VK_Init_Ext2 = NVSDK_NGX_Result(RR_NGX_MSABI*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_VK_Init_ProjectID = NVSDK_NGX_Result(RR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_VK_Init_ProjectID_Ext = NVSDK_NGX_Result(RR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_VK_GetFeatureReq = NVSDK_NGX_Result(RR_NGX_MSABI*)(VkInstance, VkPhysicalDevice, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
using PFN_VK_CreateFeature1 = NVSDK_NGX_Result(RR_NGX_MSABI*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_VK_CreateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_VK_EvaluateFeature = NVSDK_NGX_Result(RR_NGX_MSABI*)(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
using PFN_VK_Shutdown1 = NVSDK_NGX_Result(RR_NGX_MSABI*)(VkDevice);

PFN_VK_Init real_vk_init = nullptr;
PFN_VK_Init_Ext real_vk_init_ext = nullptr;
PFN_VK_Init_Ext2 real_vk_init_ext2 = nullptr;
PFN_VK_Init_ProjectID real_vk_init_project = nullptr;
PFN_VK_Init_ProjectID_Ext real_vk_init_project_ext = nullptr;
PFN_GetParams real_vk_get_cap_params = nullptr;
PFN_GetParams real_vk_get_params = nullptr;
PFN_GetParams real_vk_alloc_params = nullptr;
PFN_PopulateParams real_vk_populate_params = nullptr;
PFN_VK_GetFeatureReq real_vk_get_feat_req = nullptr;
PFN_GetScratchSize real_vk_get_scratch = nullptr;
PFN_VK_CreateFeature1 real_vk_create1 = nullptr;
PFN_VK_CreateFeature real_vk_create = nullptr;
PFN_VK_EvaluateFeature real_vk_evaluate = nullptr;
PFN_ReleaseFeature real_vk_release = nullptr;
PFN_Shutdown real_vk_shutdown = nullptr;
PFN_VK_Shutdown1 real_vk_shutdown1 = nullptr;

std::mutex g_vk_ngx_mutex;
VkInstance g_ngx_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_ngx_vk_phys = VK_NULL_HANDLE;
VkDevice g_ngx_vk_device = VK_NULL_HANDLE;

void note_ngx_vk(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, const NVSDK_NGX_FeatureCommonInfo* info) {
    ensure_deferred_init();
    note_feature_common_info(info);
    std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
    if (inst) g_ngx_vk_instance = inst;
    if (phys) g_ngx_vk_phys = phys;
    if (dev) g_ngx_vk_device = dev;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_init(unsigned long long app, const wchar_t* path, VkInstance inst, VkPhysicalDevice phys,
                                         VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                                         const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version ver) {
    note_ngx_vk(inst, phys, dev, info);
    return real_vk_init(app, path, inst, phys, dev, gipa, gdpa, info, ver);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_init_ext(unsigned long long app, const wchar_t* path, VkInstance inst, VkPhysicalDevice phys,
                                             VkDevice dev, NVSDK_NGX_Version ver, const NVSDK_NGX_FeatureCommonInfo* info) {
    note_ngx_vk(inst, phys, dev, info);
    return real_vk_init_ext(app, path, inst, phys, dev, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_init_ext2(unsigned long long app, const wchar_t* path, VkInstance inst, VkPhysicalDevice phys,
                                              VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                                              NVSDK_NGX_Version ver, const NVSDK_NGX_FeatureCommonInfo* info) {
    note_ngx_vk(inst, phys, dev, info);
    return real_vk_init_ext2(app, path, inst, phys, dev, gipa, gdpa, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_init_project(const char* proj, int eng, const char* ver_str, const wchar_t* path,
                                                 VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                                                 PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                                                 NVSDK_NGX_Version ver, const NVSDK_NGX_FeatureCommonInfo* info) {
    g_ngx_engine.store(eng);
    note_ngx_vk(inst, phys, dev, info);
    return real_vk_init_project(proj, eng, ver_str, path, inst, phys, dev, gipa, gdpa, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_init_project_ext(const char* proj, int eng, const char* ver_str, const wchar_t* path,
                                                     VkInstance inst, VkPhysicalDevice phys, VkDevice dev,
                                                     PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                                                     NVSDK_NGX_Version ver, const NVSDK_NGX_FeatureCommonInfo* info) {
    g_ngx_engine.store(eng);
    note_ngx_vk(inst, phys, dev, info);
    return real_vk_init_project_ext(proj, eng, ver_str, path, inst, phys, dev, gipa, gdpa, ver, info);
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_get_cap_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_vk_get_cap_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, true);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_get_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_vk_get_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, true);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_alloc_params(NVSDK_NGX_Parameter** out) {
    const NVSDK_NGX_Result r = real_vk_alloc_params(out);
    if (r == NVSDK_NGX_Result_Success && out && *out) patch_capability_params(*out, true);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_populate_params(NVSDK_NGX_Parameter* params) {
    const NVSDK_NGX_Result r = real_vk_populate_params(params);
    if (r == NVSDK_NGX_Result_Success && params) patch_capability_params(params, true);
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_get_feat_req(VkInstance inst, VkPhysicalDevice phys,
                                                 const NVSDK_NGX_FeatureDiscoveryInfo* info,
                                                 NVSDK_NGX_FeatureRequirement* out) {
    ensure_deferred_init();
    (void)inst;
    if (info) note_feature_common_info(info->FeatureInfo);
    const NVSDK_NGX_Result r = real_vk_get_feat_req ? real_vk_get_feat_req(inst, phys, info, out)
                                                    : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (info && info->FeatureID == NVSDK_NGX_Feature_RayReconstruction && out && vk_rr_capable(phys)) {
        out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        out->MinHWArchitecture = 0;
        std::snprintf(out->MinOSVersion, sizeof(out->MinOSVersion), "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }
    return r;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_get_scratch(NVSDK_NGX_Feature feat, const NVSDK_NGX_Parameter* params, size_t* out) {
    if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (feat == NVSDK_NGX_Feature_RayReconstruction && vk_rr_capable()) {
        *out = 0;
        return NVSDK_NGX_Result_Success;
    }
    return real_vk_get_scratch ? real_vk_get_scratch(feat, params, out) : NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result create_vk_rr_feature(VkDevice dev, VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    ensure_deferred_init();
    (void)cmd;
    if (!params || !out) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    openrr::pe::vkdevice::DeviceInfo di{};
    if (!openrr::pe::vkdevice::get_device_info(dev, &di) || !di.prepared) {
        log("[openrr] Vulkan CreateFeature: device %p was not prepared with runtime extensions/queue", static_cast<void*>(dev));
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    if (!di.instance) {
        std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
        di.instance = g_ngx_vk_instance;
    }
    if (!vk_rr_capable(di.physical)) {
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    probe_coexist_mochi();

    auto ctx = std::make_unique<RrContext>();
    ctx->is_vk = true;
    ctx->vk_device = di.device;

    read_create_sizes(params, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h);
    if (!ctx->max_render_w || !ctx->max_render_h) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    int flags = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success)
        ctx->create_flags = uint32_t(flags);

    int depth_val = 0;
    if (params->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &depth_val) == NVSDK_NGX_Result_Success) {
        ctx->has_ngx_depth_type = true;
        ctx->ngx_hw_depth = (depth_val == NVSDK_NGX_DLSS_Depth_Type_HW);
        g_ngx_depth_seen = true;
        g_ngx_depth_hw = ctx->ngx_hw_depth;
    } else if (g_ngx_depth_seen) {
        ctx->has_ngx_depth_type = true;
        ctx->ngx_hw_depth = g_ngx_depth_hw;
    }

    int rough_val = 0;
    if (params->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &rough_val) == NVSDK_NGX_Result_Success) {
        ctx->roughness_packed = (rough_val == NVSDK_NGX_DLSS_Roughness_Mode_Packed) ? 1 : 0;
        g_ngx_roughness_seen = true;
        g_ngx_roughness_packed = ctx->roughness_packed;
    } else if (g_ngx_roughness_seen) {
        ctx->roughness_packed = g_ngx_roughness_packed;
    }

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
    const int32_t rc = g_api.Create(&ci, &ctx->runtime_ctx);
    if (rc != ONR_RR_OK || !ctx->runtime_ctx) {
        const std::string err = runtime_last_error();
        log("[openrr] Vulkan Create failed (rc=%d): %s", rc, err.c_str());
        block_session("Vulkan Create failed: " + err);
        return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }

    const openrr::pe::Config cfg = openrr::pe::current_config();
    ctx->mode_applied = cfg.mode;
    OnrRrSetFastMode(ctx->runtime_ctx, cfg.mode == "new" ? 1 : 0);

    const unsigned int id = g_next_rr_handle.fetch_add(1);
    ctx->handle_id = id;
    if (*out == nullptr) {
        ctx->allocated_handle = new NVSDK_NGX_Handle{id};
        *out = ctx->allocated_handle;
    } else {
        (*out)->Id = id;
    }

    log("[openrr] Vulkan Ray Reconstruction feature created (handle=0x%X, render=%ux%u -> output=%ux%u)",
        id, ctx->max_render_w, ctx->max_render_h, ctx->output_w, ctx->output_h);

    update_status_from_context(*ctx, false);
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        g_vk_rr[id] = std::move(ctx);
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_create1(VkDevice dev, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                            NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    if (feature == NVSDK_NGX_Feature_RayReconstruction && openrr::pe::current_config().enabled && vk_rr_capable()) {
        const NVSDK_NGX_Result r = create_vk_rr_feature(dev, cmd, params, out);
        if (r == NVSDK_NGX_Result_Success) return r;
    }
    return real_vk_create1 ? real_vk_create1(dev, cmd, feature, params, out) : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_create(VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                           NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    if (feature == NVSDK_NGX_Feature_RayReconstruction && openrr::pe::current_config().enabled && vk_rr_capable()) {
        VkDevice dev = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_vk_ngx_mutex);
            dev = g_ngx_vk_device;
        }
        const NVSDK_NGX_Result r = create_vk_rr_feature(dev, cmd, params, out);
        if (r == NVSDK_NGX_Result_Success) return r;
    }
    return real_vk_create ? real_vk_create(cmd, feature, params, out) : NVSDK_NGX_Result_Fail;
}

std::atomic<uint64_t> g_vk_skipped_frames{0};

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle,
                                             NVSDK_NGX_Parameter* params, void* callback) {
    if (!handle) return real_vk_evaluate(cmd, handle, params, callback);
    std::unique_lock<std::mutex> lock(g_ctx_mutex);
    auto it = g_vk_rr.find(handle->Id);
    if (it == g_vk_rr.end()) {
        lock.unlock();
        return real_vk_evaluate(cmd, handle, params, callback);
    }
    if (!cmd || !params) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    RrContext& ctx = *it->second;
    const openrr::pe::Config cfg = openrr::pe::current_config();
    if (cfg.mode != ctx.mode_applied) {
        ctx.mode_applied = cfg.mode;
        OnrRrSetFastMode(ctx.runtime_ctx, cfg.mode == "new" ? 1 : 0);
    }

    OnrRrFrame frame{};
    if (!fill_vk_frame(ctx, *params, frame)) return NVSDK_NGX_Result_Fail;

    const int32_t rc = g_api5.RecordVk(ctx.runtime_ctx, &frame, cmd);
    if (rc == ONR_RR_BUSY) {
        const uint64_t skip_n = ++g_vk_skipped_frames;
        if (skip_n == 1 || (skip_n & (skip_n - 1)) == 0) {
            log("[openrr] Vulkan: all 64 recording slots in flight; skipping this frame (output not written, count=%llu)",
                static_cast<unsigned long long>(skip_n));
        }
        return NVSDK_NGX_Result_Success;
    }
    if (rc != ONR_RR_OK) {
        log("[openrr] RecordVk failed (rc=%d): %s", rc, runtime_last_error().c_str());
        return NVSDK_NGX_Result_Fail;
    }

    ++ctx.frame_count;
    if (ctx.frame_count == 1 || ctx.frame_count % 30 == 0) {
        update_status_from_context(ctx, false);
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_release(NVSDK_NGX_Handle* handle) {
    if (!handle) return real_vk_release(handle);
    std::unique_ptr<RrContext> gone;
    {
        std::lock_guard<std::mutex> lock(g_ctx_mutex);
        auto it = g_vk_rr.find(handle->Id);
        if (it != g_vk_rr.end()) {
            gone = std::move(it->second);
            g_vk_rr.erase(it);
        }
    }
    if (!gone) return real_vk_release(handle);
    log("[openrr] Vulkan Ray Reconstruction feature released (handle=0x%X)", gone->handle_id);
    if (gone->runtime_ctx) g_api.Destroy(gone->runtime_ctx);
    if (gone->allocated_handle == handle) delete gone->allocated_handle;
    openrr::pe::control::Status st{};
    openrr::pe::control::report(st);
    return NVSDK_NGX_Result_Success;
}

void destroy_all_vk_rr(VkDevice filter_dev = VK_NULL_HANDLE) {
    std::vector<std::unique_ptr<RrContext>> list;
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
    }
    for (auto& ctx : list) {
        if (!ctx) continue;
        if (ctx->vk_device) {
            openrr::pe::vulkan_entry_points();
            vkDeviceWaitIdle(ctx->vk_device);
        }
        if (ctx->runtime_ctx) g_api.Destroy(ctx->runtime_ctx);
        if (ctx->allocated_handle) delete ctx->allocated_handle;
    }
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_shutdown() {
    destroy_all_vk_rr();
    return real_vk_shutdown ? real_vk_shutdown() : NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result RR_NGX_MSABI hk_vk_shutdown1(VkDevice dev) {
    destroy_all_vk_rr(dev);
    return real_vk_shutdown1 ? real_vk_shutdown1(dev) : NVSDK_NGX_Result_Success;
}

// ---- Vulkan Device & Swapchain Hooks (vkCreateDevice, vkDestroyDevice, vkQueuePresentKHR, Item #3 & #9) ----
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

struct SeenVkDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::vector<uint32_t> families;
    std::vector<uint32_t> counts;
};
std::vector<SeenVkDevice> g_seen_vk_devices;

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    const VkResult r = real_vk_create_instance(ci, a, out);
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
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && instance && list && count) {
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        g_last_vk_instance.store(instance);
        for (uint32_t i = 0; i < *count; ++i) g_phys_to_inst[list[i]] = instance;
    }
    return r;
}

VkResult do_vk_create_device(PFN_vkCreateDevice create, VkInstance inst, VkPhysicalDevice phys,
                             const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
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

    openrr::pe::vkdevice::DeviceInfo di{};
    const VkResult r = openrr::pe::vkdevice::create_with_runtime(create, inst, api_ver, phys, ci, a, out, &di);
    if (r == VK_SUCCESS && out && *out && ci) {
        SeenVkDevice sd{};
        sd.instance = di.instance;
        sd.physical = phys;
        sd.device = *out;
        for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
            sd.families.push_back(ci->pQueueCreateInfos[i].queueFamilyIndex);
            sd.counts.push_back(ci->pQueueCreateInfos[i].queueCount);
        }
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        g_seen_vk_devices.push_back(std::move(sd));
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                   const VkAllocationCallbacks* a, VkDevice* out) {
    return do_vk_create_device(g_next_vk_create_device.load(), g_last_vk_instance.load(), phys, ci, a, out);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                          const VkAllocationCallbacks* a, VkDevice* out) {
    if (t_in_vk_create_device) return real_vk_export_create_device(phys, ci, a, out);
    return do_vk_create_device(real_vk_export_create_device, VK_NULL_HANDLE, phys, ci, a, out);
}

void on_vk_device_destroying(VkDevice device) {
    if (!device) return;
    openrr::pe::menu::vulkan_forget(device);
    destroy_all_vk_rr(device);
    OnrRrNotifyVkDeviceDestroyed(device);
    openrr::pe::vkdevice::forget_device(device);
    std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
    g_seen_vk_devices.erase(std::remove_if(g_seen_vk_devices.begin(), g_seen_vk_devices.end(),
                                           [device](const SeenVkDevice& d) { return d.device == device; }),
                            g_seen_vk_devices.end());
}

VKAPI_ATTR void VKAPI_CALL hk_vk_destroy_device(VkDevice device, const VkAllocationCallbacks* a) {
    on_vk_device_destroying(device);
    real_vk_destroy_device(device, a);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_destroy_device(VkDevice device, const VkAllocationCallbacks* a) {
    on_vk_device_destroying(device);
    if (auto fn = g_gdpa_destroy_device.load()) fn(device, a);
}

// Command buffer / pool reset hooks for Vulkan retirement proofs (Item #3)
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_alloc_cbs(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    const VkResult r = real_vk_export_alloc_cbs(dev, ai, out);
    if (r == VK_SUCCESS && ai && out)
        OnrRrNotifyVkCommandBuffersAllocated(dev, ai->commandPool, ai->commandBufferCount, reinterpret_cast<void* const*>(out));
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_alloc_cbs(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    const VkResult r = g_gdpa_alloc_cbs.load()(dev, ai, out);
    if (r == VK_SUCCESS && ai && out)
        OnrRrNotifyVkCommandBuffersAllocated(dev, ai->commandPool, ai->commandBufferCount, reinterpret_cast<void* const*>(out));
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    if (cb) OnrRrNotifyVkCommandBufferReset(cb);
    return real_vk_export_begin_cb(cb, bi);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    if (cb) OnrRrNotifyVkCommandBufferReset(cb);
    return g_gdpa_begin_cb.load()(cb, bi);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    if (cb) OnrRrNotifyVkCommandBufferReset(cb);
    return real_vk_export_reset_cb(cb, flags);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    if (cb) OnrRrNotifyVkCommandBufferReset(cb);
    return g_gdpa_reset_cb.load()(cb, flags);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_export_free_cbs(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs) {
    if (cbs && count) OnrRrNotifyVkCommandBuffersFreed(dev, pool, count, reinterpret_cast<void* const*>(const_cast<VkCommandBuffer*>(cbs)));
    real_vk_export_free_cbs(dev, pool, count, cbs);
}
VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_free_cbs(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs) {
    if (cbs && count) OnrRrNotifyVkCommandBuffersFreed(dev, pool, count, reinterpret_cast<void* const*>(const_cast<VkCommandBuffer*>(cbs)));
    g_gdpa_free_cbs.load()(dev, pool, count, cbs);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_export_reset_pool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    if (pool) OnrRrNotifyVkCommandPoolReset(dev, pool);
    return real_vk_export_reset_pool(dev, pool, flags);
}
VKAPI_ATTR VkResult VKAPI_CALL hk_vk_gdpa_reset_pool(VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    if (pool) OnrRrNotifyVkCommandPoolReset(dev, pool);
    return g_gdpa_reset_pool.load()(dev, pool, flags);
}

VKAPI_ATTR void VKAPI_CALL hk_vk_export_destroy_pool(VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks* a) {
    if (pool) OnrRrNotifyVkCommandPoolDestroyed(dev, pool);
    real_vk_export_destroy_pool(dev, pool, a);
}
VKAPI_ATTR void VKAPI_CALL hk_vk_gdpa_destroy_pool(VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks* a) {
    if (pool) OnrRrNotifyVkCommandPoolDestroyed(dev, pool);
    g_gdpa_destroy_pool.load()(dev, pool, a);
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
std::atomic<bool> g_vk_present_hooks_done{false};

void hook_device_creation() {
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
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

// ---- Vulkan Swapchains & Present Heartbeat ----
struct VkSwapRec {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    std::vector<VkImage> images;
};

std::mutex g_vk_swap_mutex;
std::unordered_map<VkSwapchainKHR, VkSwapRec> g_vk_swaps;
PFN_vkCreateSwapchainKHR real_vk_create_swapchain = nullptr;
PFN_vkDestroySwapchainKHR real_vk_destroy_swapchain = nullptr;
PFN_vkQueuePresentKHR real_vk_queue_present = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                      const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR mod{};
    if (ci) {
        mod = *ci;
        mod.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    VkResult r = real_vk_create_swapchain(device, ci ? &mod : ci, a, out);
    if (r != VK_SUCCESS && ci) r = real_vk_create_swapchain(device, ci, a, out);
    if (r == VK_SUCCESS && out && *out && ci) {
        std::lock_guard<std::mutex> lock(g_vk_swap_mutex);
        g_vk_swaps[*out] = {device, ci->imageFormat, ci->imageColorSpace, ci->imageExtent,
                            ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, {}};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hk_vk_destroy_swapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    if (sc) {
        openrr::pe::menu::vulkan_forget(device, sc);
        std::lock_guard<std::mutex> lock(g_vk_swap_mutex);
        g_vk_swaps.erase(sc);
    }
    real_vk_destroy_swapchain(device, sc, a);
}

const VkPresentInfoKHR* try_vk_present_menu(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!info || info->swapchainCount != 1 || !info->pSwapchains) return nullptr;
    const VkSwapchainKHR sc = info->pSwapchains[0];
    VkSwapRec rec{};
    {
        std::lock_guard<std::mutex> lock(g_vk_swap_mutex);
        auto it = g_vk_swaps.find(sc);
        if (it == g_vk_swaps.end()) return nullptr;
        rec = it->second;
    }
    if (!(rec.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return nullptr;

    SeenVkDevice dev{};
    {
        std::lock_guard<std::mutex> lock(g_vk_dev_mutex);
        for (const auto& d : g_seen_vk_devices) {
            if (d.device == rec.device) { dev = d; break; }
        }
    }
    if (!dev.device) return nullptr;

    openrr::pe::vulkan_entry_points();
    uint32_t family = ~0u;
    for (size_t i = 0; i < dev.families.size() && family == ~0u; ++i) {
        for (uint32_t j = 0; j < (i < dev.counts.size() ? dev.counts[i] : 1u); ++j) {
            VkQueue q = VK_NULL_HANDLE;
            vkGetDeviceQueue(dev.device, dev.families[i], j, &q);
            if (q == queue) { family = dev.families[i]; break; }
        }
    }
    if (family == ~0u) return nullptr;

    if (rec.images.empty()) {
        uint32_t count = 0;
        if (vkGetSwapchainImagesKHR(rec.device, sc, &count, nullptr) != VK_SUCCESS || !count) return nullptr;
        rec.images.resize(count);
        if (vkGetSwapchainImagesKHR(rec.device, sc, &count, rec.images.data()) != VK_SUCCESS) return nullptr;
        std::lock_guard<std::mutex> lock(g_vk_swap_mutex);
        g_vk_swaps[sc].images = rec.images;
    }
    return openrr::pe::menu::vulkan_present(dev.instance, dev.physical, dev.device, queue, family, sc,
                                            rec.format, rec.space, rec.extent, rec.images, info);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_vk_queue_present(VkQueue q, const VkPresentInfoKHR* p) {
    ensure_deferred_init();
    openrr::pe::watchdog::beat();
    if (const VkPresentInfoKHR* mine = try_vk_present_menu(q, p)) p = mine;
    return real_vk_queue_present(q, p);
}

void hook_present_beat() {
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
    if (!m) return;
    if (g_vk_present_hooks_done.exchange(true, std::memory_order_acq_rel)) return;
    auto try_hk = [&](const char* name, void* detour, void** orig) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
        if (fn && MH_CreateHook(fn, detour, orig) == MH_OK) MH_EnableHook(fn);
    };
    try_hk("vkCreateSwapchainKHR", reinterpret_cast<void*>(&hk_vk_create_swapchain), reinterpret_cast<void**>(&real_vk_create_swapchain));
    try_hk("vkDestroySwapchainKHR", reinterpret_cast<void*>(&hk_vk_destroy_swapchain), reinterpret_cast<void**>(&real_vk_destroy_swapchain));
    try_hk("vkQueuePresentKHR", reinterpret_cast<void*>(&hk_vk_queue_present), reinterpret_cast<void**>(&real_vk_queue_present));
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
            openrr::pe::menu::on_present_beat(&on_dxgi_present_cb);
            openrr::pe::menu::prepare_dxgi();
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

// ---- Streamline sl.dlss_d.dll load adaptation (DXGI spoofing off, e.g. OptiPatcher) ----
// sl.dlss_d offers RR only if sl.common's SystemCaps report a supported NVIDIA adapter. During its synchronous
// slOnPluginLoad (slInit, before any device or present work) adapters OpenRR can serve are shown as NVIDIA in the
// caps object sl.common shares with it; every touched field is restored on return. This covers DLSS-D's own read and
// the sl.common callbacks it calls from there (NGXGetFeatureRequirements, updateCommonEmbeddedJSONConfig).
// Known limits: no other thread is expected to read the caps during slInit's plugin load, and callbacks a plugin
// might defer past slOnPluginLoad see the real caps.
// sl::param::IParameters with the MSVC ABI: vtable[7] = get(const char*, void**).
struct SlParametersVtable { void* slots[15]; };
struct SlParameters { const SlParametersVtable* vtable; };
struct SlAdapter {
    LUID id{};
    uint32_t vendor{};
    uint32_t bit{};
    uint32_t architecture{};
    uint32_t implementation{};
    uint32_t revision{};
    uint32_t deviceId{};
    void* nativeInterface{};
};
constexpr uint32_t kSlMaxNumSupportedGPUs = 8;
struct SlSystemCaps {
    uint32_t gpuCount{};
    uint32_t osVersionMajor{};
    uint32_t osVersionMinor{};
    uint32_t osVersionBuild{};
    uint32_t driverVersionMajor{};
    uint32_t driverVersionMinor{};
    SlAdapter adapters[kSlMaxNumSupportedGPUs]{};
    uint32_t gpuLoad[kSlMaxNumSupportedGPUs]{};
    bool hwsSupported{};
    bool laptopDevice{};
};

using PFN_slGetPluginFunction = void*(RR_NGX_MSABI*)(const char*);
using PFN_slOnPluginLoad = bool(RR_NGX_MSABI*)(SlParameters*, const char*, const char**);
std::atomic<void*> g_dlssd_hook_target{nullptr};
std::atomic<PFN_slGetPluginFunction> g_real_dlssd_get{nullptr};
std::atomic<PFN_slOnPluginLoad> g_real_dlssd_load{nullptr};

bool sl_caps_usable(const SlSystemCaps* c) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!c || !VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        reinterpret_cast<uintptr_t>(c) + sizeof(*c) > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
        return false;
    return c->gpuCount > 0 && c->gpuCount <= kSlMaxNumSupportedGPUs && c->osVersionMajor == 10;
}

struct SlCapsPatch {
    SlSystemCaps* caps = nullptr;
    uint32_t mask = 0;
    uint32_t vendor[kSlMaxNumSupportedGPUs]{}, architecture[kSlMaxNumSupportedGPUs]{};
    uint32_t driver_major = 0, driver_minor = 0;
    bool hws = false;

    explicit SlCapsPatch(SlParameters* params) {
        if (!params || !params->vtable || !params->vtable->slots[7] || !openrr::pe::current_config().enabled) return;
        using GetPtr = bool(RR_NGX_MSABI*)(SlParameters*, const char*, void**);
        void* raw = nullptr;
        if (!reinterpret_cast<GetPtr>(params->vtable->slots[7])(params, "sl.param.common.gpuInfo", &raw)) return;
        auto* c = static_cast<SlSystemCaps*>(raw);
        if (!sl_caps_usable(c)) return;
        for (uint32_t i = 0; i < c->gpuCount; ++i) {
            SlAdapter& a = c->adapters[i];
            const bool candidate = a.vendor == 0x1002u || (a.vendor == 0x10DEu && a.architecture < 0x160u);
            if (!candidate || !(a.id.LowPart || a.id.HighPart) || !check_usable(&a.id)) continue;
            vendor[i] = a.vendor;
            architecture[i] = a.architecture;
            mask |= 1u << i;
            a.vendor = 0x10DEu;
            if (a.architecture < 0x160u) a.architecture = 0x190u;
        }
        if (!mask) return;
        caps = c;
        driver_major = c->driverVersionMajor;
        driver_minor = c->driverVersionMinor;
        hws = c->hwsSupported;
        if (driver_major < 535u || (driver_major == 535u && driver_minor < 15u)) {
            c->driverVersionMajor = 999u;
            c->driverVersionMinor = 0u;
        }
        c->hwsSupported = true;
    }
    ~SlCapsPatch() {
        if (!caps) return;
        for (uint32_t i = 0; i < kSlMaxNumSupportedGPUs; ++i) {
            if (!(mask & (1u << i))) continue;
            caps->adapters[i].vendor = vendor[i];
            caps->adapters[i].architecture = architecture[i];
        }
        caps->driverVersionMajor = driver_major;
        caps->driverVersionMinor = driver_minor;
        caps->hwsSupported = hws;
    }
    SlCapsPatch(const SlCapsPatch&) = delete;
    SlCapsPatch& operator=(const SlCapsPatch&) = delete;
};

void remove_json_token(std::string& json, const char* token) {
    const std::string quoted = std::string("\"") + token + "\"";
    auto blank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    size_t pos = 0;
    while ((pos = json.find(quoted, pos)) != std::string::npos) {
        size_t start = pos, end = pos + quoted.size(), after = end;
        while (after < json.size() && blank(json[after])) ++after;
        if (after < json.size() && json[after] == ',') {
            end = after + 1;
        } else {
            size_t before = start;
            while (before > 0 && blank(json[before - 1])) --before;
            if (before > 0 && json[before - 1] == ',') start = before - 1;
        }
        json.erase(start, end - start);
        pos = start;
    }
}

bool RR_NGX_MSABI hk_dlssd_on_plugin_load(SlParameters* params, const char* loader_json, const char** plugin_json) {
    const PFN_slOnPluginLoad real = g_real_dlssd_load.load(std::memory_order_acquire);
    if (!real) return false;
    static thread_local bool inside = false;
    if (inside) return real(params, loader_json, plugin_json);
    struct Inside { Inside() { inside = true; } ~Inside() { inside = false; } } guard;

    uint32_t adapted = 0;
    bool result = false;
    {
        SlCapsPatch patch(params);
        adapted = patch.mask;
        result = real(params, loader_json, plugin_json);
    }
    if (!adapted) return result;
    log("[openrr] sl.dlss_d: SystemCaps adapted during slOnPluginLoad (adapter mask 0x%x) -> %s",
        adapted, result ? "loaded" : "refused");
    if (result && plugin_json && *plugin_json) {
        // OpenRR does not use NVIDIA-only Vulkan extensions; the host must not be asked to enable them.
        std::string patched = *plugin_json;
        remove_json_token(patched, "VK_NVX_binary_import");
        remove_json_token(patched, "VK_NVX_image_view_handle");
        static std::mutex mutex;
        static std::unordered_set<std::string> pool; // Returned strings stay valid for the process lifetime.
        std::lock_guard<std::mutex> lock(mutex);
        *plugin_json = pool.insert(std::move(patched)).first->c_str();
    }
    return result;
}

void* RR_NGX_MSABI hk_dlssd_get_plugin_fn(const char* name) {
    const PFN_slGetPluginFunction get = g_real_dlssd_get.load(std::memory_order_acquire);
    void* fn = get ? get(name) : nullptr;
    if (fn && name && std::strcmp(name, "slOnPluginLoad") == 0) {
        g_real_dlssd_load.store(reinterpret_cast<PFN_slOnPluginLoad>(fn), std::memory_order_release);
        return reinterpret_cast<void*>(&hk_dlssd_on_plugin_load);
    }
    return fn;
}

// sl.dlss_d exports only slGetPluginFunction; sl.interposer resolves slOnPluginLoad through it right after loading.
void hook_sl_dlssd(HMODULE mod) {
    void* fn = mod ? reinterpret_cast<void*>(GetProcAddress(mod, "slGetPluginFunction")) : nullptr;
    void* expected = nullptr;
    if (!fn || !g_dlssd_hook_target.compare_exchange_strong(expected, fn, std::memory_order_acq_rel)) return;
    const MH_STATUS init = MH_Initialize();
    void* orig = nullptr;
    if ((init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) &&
        MH_CreateHook(fn, reinterpret_cast<void*>(&hk_dlssd_get_plugin_fn), &orig) == MH_OK) {
        g_real_dlssd_get.store(reinterpret_cast<PFN_slGetPluginFunction>(orig), std::memory_order_release);
        if (MH_EnableHook(fn) == MH_OK) return;
        MH_RemoveHook(fn);
    }
    g_real_dlssd_get.store(nullptr, std::memory_order_release);
    g_dlssd_hook_target.store(nullptr, std::memory_order_release);
    log("[openrr] sl.dlss_d.dll: slGetPluginFunction could not be hooked; Streamline RR then needs DXGI spoofing");
}

void unhook_sl_dlssd(const void* base, ULONG size) {
    void* fn = g_dlssd_hook_target.load(std::memory_order_acquire);
    const auto at = reinterpret_cast<uintptr_t>(fn), begin = reinterpret_cast<uintptr_t>(base);
    if (!fn || at < begin || at - begin >= size) return;
    MH_RemoveHook(fn); // The image is still mapped while the unload notification runs.
    g_real_dlssd_load.store(nullptr, std::memory_order_release);
    g_real_dlssd_get.store(nullptr, std::memory_order_release);
    g_dlssd_hook_target.store(nullptr, std::memory_order_release);
}

// Late vulkan-1.dll / d3d12.dll load notification via LdrRegisterDllNotification (Item #9)
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
    if (chars == 13 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"sl.dlss_d.dll", 13) == 0) {
        if (reason == 1) hook_sl_dlssd(static_cast<HMODULE>(data->Loaded.DllBase));
        else unhook_sl_dlssd(data->Loaded.DllBase, data->Loaded.SizeOfImage);
        return;
    }
    if (reason != 1) return;
    if (chars == 12 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"vulkan-1.dll", 12) == 0) {
        hook_device_creation();
        hook_present_beat();
    } else if (chars == 9 && _wcsnicmp(data->Loaded.BaseDllName->Buffer, L"d3d12.dll", 9) == 0) {
        hook_d3d12_device_creation();
    }
}

void register_vulkan_dll_notification() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg) reg(0, &on_dll_loaded_notification, nullptr, &g_dll_notif_cookie);
}

// ---- Present Heartbeat Callbacks (registered with openrr::pe::menu) ----
void on_dxgi_present_cb() {
    ensure_deferred_init();
    openrr::pe::watchdog::beat();
}

// ---- Process Termination Watch & Hook Helper ----
using PFN_NtTerminateProcess = LONG(NTAPI*)(HANDLE, LONG);
PFN_NtTerminateProcess real_terminate = nullptr;
std::atomic<bool> g_terminate_said{false}, g_exiting{false};

LONG NTAPI hk_terminate(HANDLE process, LONG status) {
    const bool self = process == GetCurrentProcess() || (process && GetProcessId(process) == GetCurrentProcessId());
    if (self && !g_exiting.load() && !g_terminate_said.exchange(true)) {
        log("[openrr] process is being ended by TerminateProcess (exit status 0x%08lX), thread %lu",
            static_cast<unsigned long>(status), GetCurrentThreadId());
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

bool hook(const char* name, void* detour, void** real, bool required = false) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(g_opti, name));
    if (!fn) {
        if (required) log("[openrr] OptiScaler exports no %s", name);
        return false;
    }
    // NR's relay is private executable memory; the detour it points at belongs to NR.
    void* target = fn;
    if (openrr::pe::readable_code(fn, 5) && *static_cast<uint8_t*>(fn) == 0xe9) {
        int32_t rel = 0;
        std::memcpy(&rel, static_cast<uint8_t*>(fn) + 1, sizeof(rel));
        const auto relay = reinterpret_cast<uintptr_t>(fn) + 5 + rel;
        const auto next = openrr::pe::absolute_jump(reinterpret_cast<uint8_t*>(relay));
        HMODULE owner = nullptr;
        if (next && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(next), &owner)) {
            auto base = std::filesystem::path(openrr::u8::module_file(owner)).filename().string();
            for (char& c : base) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (base.starts_with("dlssnr-amd") && base.ends_with(".asi")) {
                const Range range = range_of(owner);
                if (void* inner = openrr::pe::minhook_inner_target(fn, range.begin, range.end)) {
                    target = inner;
                    log("[openrr] %s: NR wrapper preserved; hooking trampoline %p (export %p)", name, target, fn);
                } else {
                    log("[openrr] %s: unsupported NR trampoline; ordinary hook fallback (NR before/after compatibility NOT enabled)", name);
                }
            }
        }
    }
    const MH_STATUS c = MH_CreateHook(target, detour, real);
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(target) : c;
    if (e != MH_OK) log("[openrr] %s could not be hooked (%s)", name, MH_StatusToString(e));
    return e == MH_OK;
}

#ifdef OPENRR_EXPERIMENTAL_OPTIFG_HOST
#include "optifg_provider_locator.hpp"
bool prepare_optifg_host() {
    static std::once_flag once;
    std::call_once(once, [] {
        char flag[16]{};
        const auto ini = std::string(openrr::pe::module_folder()) + "\\openrr-amd.ini";
        openrr::u8::profile("OpenRR", "OptiFGHost", "false", flag, sizeof(flag), ini.c_str());
        if (_stricmp(flag, "true") && std::strcmp(flag, "1")) return;
        auto at=optifg_probe::locate(g_opti);
        if (!at) { log("[openrr-host] locator rejected; independent ASI retained"); return; }
        auto path=std::filesystem::path(openrr::u8::module_file(g_opti)).parent_path()/L"openrr-ngx-host.dll";
        // NvngxPath must have loaded the companion into NVNGXProxy already.
        // Loading it here would not establish that binding.
        auto proxy=GetModuleHandleW(path.c_str());
        // OptiDllPath / NvngxPath may place the companion in another directory.
        // Retain the marker and export checks below for either lookup.
        if (!proxy) proxy=GetModuleHandleW(L"openrr-ngx-host.dll");
        using Marker = unsigned int (*)();
        auto marker = proxy ? reinterpret_cast<Marker>(GetProcAddress(proxy, "OpenRrNgxProxyMarker")) : nullptr;
        if (!marker || marker() != 0x4f520001u ||
            !GetProcAddress(proxy, "NVSDK_NGX_D3D12_CreateFeature") ||
            !GetProcAddress(proxy, "NVSDK_NGX_D3D12_EvaluateFeature") ||
            !GetProcAddress(proxy, "NVSDK_NGX_D3D12_ReleaseFeature")) {
            log("[openrr-host] compatible openrr-ngx-host.dll not loaded; check Libraries.NvngxPath; independent ASI retained"); return;
        }
        DWORD old=0;
        if (!VirtualProtect(at,6,PAGE_EXECUTE_READWRITE,&old)) return;
        std::memset(at,0x90,6);
        DWORD ignored; VirtualProtect(at,6,old,&ignored); FlushInstructionCache(GetCurrentProcess(),at,6);
        g_optifg_host_active=true;
        log("[openrr-host] EXPERIMENT enabled; capability branch bypassed, path branch preserved");
    });
    return g_optifg_host_active;
}
#endif
} // namespace

// Called by OptiScaler from its DllMain, under the loader lock: hooks and INI read only (Item #8).
#ifdef OPENRR_EXPERIMENTAL_OPTIFG_HOST
// Prototype-only inner NGX endpoints. Never call outer NGX trampolines on failure.
extern "C" __declspec(dllexport) NVSDK_NGX_Result RR_NGX_MSABI OpenRrHostCreateD3D12(
    ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out) {
    try { return create_d3d12_rr(cmd, feature, params, out, true); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result RR_NGX_MSABI OpenRrHostEvaluateD3D12(
    ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params, void* callback) {
    try { return evaluate_d3d12_rr(cmd, handle, params, callback, true); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result RR_NGX_MSABI OpenRrHostReleaseD3D12(NVSDK_NGX_Handle* handle) {
    try { return release_d3d12_rr(handle, true); }
    catch (...) { return NVSDK_NGX_Result_Fail; }
}
#endif

extern "C" __declspec(dllexport) void InitializeASI() {
    static bool done = false;
    if (done) return;
    done = true;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &g_opti);
    if (!g_opti) g_opti = GetModuleHandleW(nullptr);
    g_opti_range = range_of(g_opti);
    g_self_range = range_of(g_self);

    openrr::pe::set_module(g_opti);
    openrr::pe::install_crash_watch();
    std::set_terminate([] {
        log("[openrr] std::terminate: an unhandled exception terminated an OpenRR thread");
        std::abort();
    });

    log("[openrr] openrr-amd.asi v%s (%s) loaded by %s",
        ONR_RR_RUNTIME_VERSION, OPENRR_BUILD_STAMP, openrr::u8::module_file(g_opti).c_str());

    judge_executable();
    read_ini_barriers();
    probe_coexist_mochi();
    (void)openrr::pe::current_config();

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("[openrr] MinHook could not start (%s): OpenRR does not run", MH_StatusToString(init));
        return;
    }
    watch_termination();

    // D3D12 NGX hooks
    const bool d3d12_ok =
        hook("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&hk_d3d12_create), reinterpret_cast<void**>(&real_d3d12_create), true) &&
        hook("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&hk_d3d12_evaluate), reinterpret_cast<void**>(&real_d3d12_evaluate), true) &&
        hook("NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void*>(&hk_d3d12_release), reinterpret_cast<void**>(&real_d3d12_release), true);

    hook("NVSDK_NGX_D3D12_Init", reinterpret_cast<void*>(&hk_d3d12_init), reinterpret_cast<void**>(&real_d3d12_init), false);
    hook("NVSDK_NGX_D3D12_Init_Ext", reinterpret_cast<void*>(&hk_d3d12_init_ext), reinterpret_cast<void**>(&real_d3d12_init_ext), false);
    hook("NVSDK_NGX_D3D12_Init_ProjectID", reinterpret_cast<void*>(&hk_d3d12_init_project), reinterpret_cast<void**>(&real_d3d12_init_project), false);
    hook("NVSDK_NGX_D3D12_Init_with_ProjectID", reinterpret_cast<void*>(&hk_d3d12_init_with_project), reinterpret_cast<void**>(&real_d3d12_init_with_project), false);
    hook("NVSDK_NGX_D3D12_GetCapabilityParameters", reinterpret_cast<void*>(&hk_d3d12_get_cap_params), reinterpret_cast<void**>(&real_d3d12_get_cap_params), false);
    hook("NVSDK_NGX_D3D12_GetParameters", reinterpret_cast<void*>(&hk_d3d12_get_params), reinterpret_cast<void**>(&real_d3d12_get_params), false);
    hook("NVSDK_NGX_D3D12_AllocateParameters", reinterpret_cast<void*>(&hk_d3d12_alloc_params), reinterpret_cast<void**>(&real_d3d12_alloc_params), false);
    if (!hook("NVSDK_NGX_D3D12_PopulateParameters_Impl", reinterpret_cast<void*>(&hk_d3d12_populate_params), reinterpret_cast<void**>(&real_d3d12_populate_params), false)) {
        hook("NVSDK_NGX_D3D12_PopulateParameters", reinterpret_cast<void*>(&hk_d3d12_populate_params), reinterpret_cast<void**>(&real_d3d12_populate_params), false);
    }
    hook("NVSDK_NGX_D3D12_GetFeatureRequirements", reinterpret_cast<void*>(&hk_d3d12_get_feat_req), reinterpret_cast<void**>(&real_d3d12_get_feat_req), false);
    hook("NVSDK_NGX_D3D12_GetScratchBufferSize", reinterpret_cast<void*>(&hk_d3d12_get_scratch), reinterpret_cast<void**>(&real_d3d12_get_scratch), false);
    hook("NVSDK_NGX_D3D12_Shutdown", reinterpret_cast<void*>(&hk_d3d12_shutdown), reinterpret_cast<void**>(&real_d3d12_shutdown), false);
    hook("NVSDK_NGX_D3D12_Shutdown1", reinterpret_cast<void*>(&hk_d3d12_shutdown1), reinterpret_cast<void**>(&real_d3d12_shutdown1), false);

    // Vulkan NGX hooks
    const bool vk_ok =
        hook("NVSDK_NGX_VULKAN_EvaluateFeature", reinterpret_cast<void*>(&hk_vk_evaluate), reinterpret_cast<void**>(&real_vk_evaluate), false) &&
        hook("NVSDK_NGX_VULKAN_ReleaseFeature", reinterpret_cast<void*>(&hk_vk_release), reinterpret_cast<void**>(&real_vk_release), false);

    hook("NVSDK_NGX_VULKAN_CreateFeature1", reinterpret_cast<void*>(&hk_vk_create1), reinterpret_cast<void**>(&real_vk_create1), false);
    hook("NVSDK_NGX_VULKAN_CreateFeature", reinterpret_cast<void*>(&hk_vk_create), reinterpret_cast<void**>(&real_vk_create), false);
    hook("NVSDK_NGX_VULKAN_Init", reinterpret_cast<void*>(&hk_vk_init), reinterpret_cast<void**>(&real_vk_init), false);
    hook("NVSDK_NGX_VULKAN_Init_Ext", reinterpret_cast<void*>(&hk_vk_init_ext), reinterpret_cast<void**>(&real_vk_init_ext), false);
    hook("NVSDK_NGX_VULKAN_Init_Ext2", reinterpret_cast<void*>(&hk_vk_init_ext2), reinterpret_cast<void**>(&real_vk_init_ext2), false);
    hook("NVSDK_NGX_VULKAN_Init_ProjectID", reinterpret_cast<void*>(&hk_vk_init_project), reinterpret_cast<void**>(&real_vk_init_project), false);
    hook("NVSDK_NGX_VULKAN_Init_ProjectID_Ext", reinterpret_cast<void*>(&hk_vk_init_project_ext), reinterpret_cast<void**>(&real_vk_init_project_ext), false);
    hook("NVSDK_NGX_VULKAN_GetCapabilityParameters", reinterpret_cast<void*>(&hk_vk_get_cap_params), reinterpret_cast<void**>(&real_vk_get_cap_params), false);
    hook("NVSDK_NGX_VULKAN_GetParameters", reinterpret_cast<void*>(&hk_vk_get_params), reinterpret_cast<void**>(&real_vk_get_params), false);
    hook("NVSDK_NGX_VULKAN_AllocateParameters", reinterpret_cast<void*>(&hk_vk_alloc_params), reinterpret_cast<void**>(&real_vk_alloc_params), false);
    if (!hook("NVSDK_NGX_VULKAN_PopulateParameters_Impl", reinterpret_cast<void*>(&hk_vk_populate_params), reinterpret_cast<void**>(&real_vk_populate_params), false)) {
        hook("NVSDK_NGX_VULKAN_PopulateParameters", reinterpret_cast<void*>(&hk_vk_populate_params), reinterpret_cast<void**>(&real_vk_populate_params), false);
    }
    hook("NVSDK_NGX_VULKAN_GetFeatureRequirements", reinterpret_cast<void*>(&hk_vk_get_feat_req), reinterpret_cast<void**>(&real_vk_get_feat_req), false);
    hook("NVSDK_NGX_VULKAN_GetScratchBufferSize", reinterpret_cast<void*>(&hk_vk_get_scratch), reinterpret_cast<void**>(&real_vk_get_scratch), false);
    hook("NVSDK_NGX_VULKAN_Shutdown", reinterpret_cast<void*>(&hk_vk_shutdown), reinterpret_cast<void**>(&real_vk_shutdown), false);
    hook("NVSDK_NGX_VULKAN_Shutdown1", reinterpret_cast<void*>(&hk_vk_shutdown1), reinterpret_cast<void**>(&real_vk_shutdown1), false);

    hook_device_creation();
    hook_d3d12_device_creation();
    hook_present_beat();
    register_vulkan_dll_notification();
    hook_sl_dlssd(GetModuleHandleW(L"sl.dlss_d.dll")); // Normally loaded later by slInit (notification path).
    // No CreateDXGIFactory* export hooks: on OptiScaler 10 they sit on OptiScaler's own exports (which decide by
    // caller module) and make the game's swapchain creation fail. Present is reached through the menu's
    // swapchain methods; system factory methods retain the actual present queue.

    log("[openrr] NGX hooks installed (D3D12=%s, Vulkan=%s, UnrealByExe=%s)",
        d3d12_ok ? "ok" : "failed", vk_ok ? "ok" : "failed", g_unreal_by_exe ? "yes" : "no");
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        g_exiting = true;
        log("[openrr] process is ending (ExitProcess)");
        openrr::pe::remove_crash_watch();
        openrr::pe::menu::abandon_at_exit();
        for (auto& [id, ctx] : g_d3d12_rr) (void)ctx.release();
        for (auto& [id, ctx] : g_vk_rr) (void)ctx.release();
        openrr::pe::flush_log();
    } else if (reason == DLL_PROCESS_DETACH) {
        openrr::pe::remove_crash_watch();
        openrr::pe::flush_log();
    }
    return TRUE;
}
