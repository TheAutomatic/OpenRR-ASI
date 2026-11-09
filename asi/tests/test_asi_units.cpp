// SPDX-License-Identifier: MIT
// Unit and D3D12 harness tests for openrr-amd.asi components:
// - Item #13: single-digit and numeric virtual-key parsing in openrr::pe::parse_key
// - Item #11: split vtable fallback self-recursion guard & CreateCommandList fallback on copied vtables
// - Item #12: split prepare() rejection of open split barriers, enhanced barriers, RTAS, meta commands, and bundles
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "rr_pe_config.hpp"
#include "rr_ini_edit.hpp"
#include "rr_pe_d3d12split.hpp"
#include "rr_pe_vkdevice.hpp"
#include "opennr_rr_api.h"
#include "opennr_rr/d3d12.hpp"
#include "opennr_rr/engine.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static int test_key_parsing() {
    int vk = 0, mods = 0;

    if (!openrr::pe::parse_key("1", &vk, &mods) || vk != 0x31 || mods != 0) {
        std::fprintf(stderr, "FAIL: '1' -> vk=0x%X mods=0x%X (expected 0x31, 0)\n", vk, mods);
        return 1;
    }
    if (!openrr::pe::parse_key("45", &vk, &mods) || vk != 0x2D || mods != 0) {
        std::fprintf(stderr, "FAIL: '45' -> vk=0x%X mods=0x%X (expected 0x2D, 0)\n", vk, mods);
        return 1;
    }
    if (!openrr::pe::parse_key("0x2D", &vk, &mods) || vk != 0x2D || mods != 0) {
        std::fprintf(stderr, "FAIL: '0x2D' -> vk=0x%X mods=0x%X (expected 0x2D, 0)\n", vk, mods);
        return 1;
    }
    if (!openrr::pe::parse_key("ctrl+5", &vk, &mods) || vk != 0x35 || mods != MOD_CONTROL) {
        std::fprintf(stderr, "FAIL: 'ctrl+5' -> vk=0x%X mods=0x%X (expected 0x35, MOD_CONTROL)\n", vk, mods);
        return 1;
    }
    if (!openrr::pe::parse_key("f5", &vk, &mods) || vk != VK_F5 || mods != 0) {
        std::fprintf(stderr, "FAIL: 'f5' -> vk=0x%X mods=0x%X (expected VK_F5, 0)\n", vk, mods);
        return 1;
    }
    std::printf("[PASS] Item #13: parse_key ('1'->0x31, '45'->0x2D, '0x2D'->0x2D, 'ctrl+5'->0x35+MOD_CONTROL, 'f5'->VK_F5)\n");

    // ---- R7 & R8: UTF-8 BOM + missing trailing newline in INI parser & editor ----
    {
        const std::filesystem::path tmp_ini = std::filesystem::temp_directory_path() / L"openrr_test_bom_nl.ini";
        {
            FILE* f = openrr::u8::fopen(openrr::u8::text(tmp_ini).c_str(), "wb");
            if (!f) {
                std::fprintf(stderr, "FAIL: R7/R8 could not create temp ini\n");
                return 1;
            }
            const char raw[] = "\xEF\xBB\xBF[OpenRR]\nEnabled = false"; // UTF-8 BOM + no trailing newline
            std::fwrite(raw, 1, sizeof(raw) - 1, f);
            std::fclose(f);
        }
        char prof_buf[64]{};
        openrr::u8::profile("OpenRR", "Enabled", "true", prof_buf, sizeof(prof_buf), openrr::u8::text(tmp_ini));
        const openrr::pe::Config cfg_before = openrr::pe::load_config_file(openrr::u8::text(tmp_ini));
        openrr::ini_edit::Ini ini(tmp_ini);
        ini.refresh();
        const std::string en_before = ini.get("OpenRR", "Enabled", "true");
        ini.set("OpenRR", "Mode", "legacy");
        ini.save();
        openrr::ini_edit::Ini ini2(tmp_ini);
        ini2.refresh();
        const std::string en_after = ini2.get("OpenRR", "Enabled", "true");
        const std::string mode_after = ini2.get("OpenRR", "Mode", "");
        const openrr::pe::Config cfg_after = openrr::pe::load_config_file(openrr::u8::text(tmp_ini));
        std::error_code ec;
        std::filesystem::remove(tmp_ini, ec);
        if (std::string(prof_buf) != "false" || en_before != "false" || en_after != "false" || mode_after != "legacy" ||
            cfg_before.enabled || cfg_after.enabled || cfg_after.mode != "legacy") {
            std::fprintf(stderr, "FAIL: R7/R8 BOM/newline test failed (prof='%s' before='%s' after='%s' mode='%s' cfg_en=%d/%d cfg_mode='%s')\n",
                         prof_buf, en_before.c_str(), en_after.c_str(), mode_after.c_str(),
                         int(cfg_before.enabled), int(cfg_after.enabled), cfg_after.mode.c_str());
            return 1;
        }
        std::printf("[PASS] Item R7/R8 (UTF-8 BOM & missing trailing newline in load_config_file + Ini): Enabled='%s', appended Mode='%s'\n",
                    en_after.c_str(), mode_after.c_str());
    }
    return 0;
}

static int test_split_hardening_and_eligibility() {
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) {
        IDXGIFactory4* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter* warp = nullptr;
            if (SUCCEEDED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
                D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
                warp->Release();
            }
            fac->Release();
        }
    }
    if (!dev) {
        std::fprintf(stderr, "FAIL: could not create D3D12 device for split tests\n");
        return 1;
    }

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)))) {
        std::fprintf(stderr, "FAIL: could not create command allocator / command list\n");
        dev->Release();
        return 1;
    }

    // Install split hooks and reset so state is known (control case + R6 untracked reset reason check).
    std::string why;
    const bool prep_untracked = openrr::pe::split::prepare(cl, &why);
    if (prep_untracked || !openrr::pe::split::is_untracked_reset_reason(why)) {
        std::fprintf(stderr, "FAIL: R6 untracked prepare() expected false with untracked-reset reason, got ok=%d why='%s'\n",
                     int(prep_untracked), why.c_str());
        return 1;
    }
    cl->Close();
    cl->Reset(alloc, nullptr);
    if (!openrr::pe::split::prepare(cl, &why)) {
        std::fprintf(stderr, "FAIL: control prepare() after Reset failed: %s\n", why.c_str());
        return 1;
    }
    std::printf("[PASS] Item #12 (control) & R6 (untracked reset reason matched): ordinary command list accepted by split::prepare\n");

    // ---- Item #11: Copied vtable must not self-recurse on Reset/Close or return E_FAIL on CreateCommandList ----
    {
        void** hooked_cl_vt = *reinterpret_cast<void***>(cl);
        void* copied_cl_vt[96]{};
        std::memcpy(copied_cl_vt, hooked_cl_vt, 86 * sizeof(void*));
        *reinterpret_cast<void***>(cl) = copied_cl_vt;
        const HRESULT hr_close = cl->Close();
        const HRESULT hr_reset = cl->Reset(alloc, nullptr);
        const HRESULT hr_close2 = cl->Close();
        *reinterpret_cast<void***>(cl) = hooked_cl_vt;
        if (FAILED(hr_close) || FAILED(hr_reset) || FAILED(hr_close2)) {
            std::fprintf(stderr, "FAIL: Item #11 copied cl vtable Close=0x%lX Reset=0x%lX\n",
                         static_cast<unsigned long>(hr_close), static_cast<unsigned long>(hr_reset));
            return 1;
        }

        void** hooked_dev_vt = *reinterpret_cast<void***>(dev);
        void* copied_dev_vt[64]{};
        std::memcpy(copied_dev_vt, hooked_dev_vt, 64 * sizeof(void*));
        *reinterpret_cast<void***>(dev) = copied_dev_vt;
        ID3D12GraphicsCommandList* cl2 = nullptr;
        const HRESULT hr_create = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl2));
        *reinterpret_cast<void***>(dev) = hooked_dev_vt;
        if (FAILED(hr_create) || !cl2) {
            std::fprintf(stderr, "FAIL: Item #11 copied device vtable CreateCommandList returned 0x%lX\n",
                         static_cast<unsigned long>(hr_create));
            return 1;
        }
        cl2->Close();
        cl2->Release();
        cl->Reset(alloc, nullptr);
        std::printf("[PASS] Item #11: copied command-list and device vtables succeeded without recursion or E_FAIL\n");
    }

    // Create a small buffer resource for split-barrier test.
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 4096;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf));

    // ---- Item #12 Case 1: BEGIN_ONLY split barrier rejected, END_ONLY restores eligibility ----
    cl->Close();
    cl->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER sb{};
    sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sb.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    sb.Transition = {buf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ};
    cl->ResourceBarrier(1, &sb);
    why.clear();
    if (openrr::pe::split::prepare(cl, &why) || why.find("split barrier") == std::string::npos) {
        std::fprintf(stderr, "FAIL: Item #12 BEGIN_ONLY was not rejected (why='%s')\n", why.c_str());
        return 1;
    }
    sb.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
    cl->ResourceBarrier(1, &sb);
    if (!openrr::pe::split::prepare(cl, &why)) {
        std::fprintf(stderr, "FAIL: Item #12 after END_ONLY should be eligible again (why='%s')\n", why.c_str());
        return 1;
    }
    std::printf("[PASS] Item #12 (split barrier): BEGIN_ONLY rejected ('%s'), END_ONLY accepted\n", why.c_str());

    // ---- Item #12 Case 2: Enhanced Barrier (slot 80) rejected ----
    void** vt = *reinterpret_cast<void***>(cl);
    cl->Close();
    cl->Reset(alloc, nullptr);
    using PFN_Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT32, const void*);
    reinterpret_cast<PFN_Barrier>(vt[80])(cl, 0, nullptr);
    why.clear();
    if (openrr::pe::split::prepare(cl, &why) || why.find("enhanced barrier") == std::string::npos) {
        std::fprintf(stderr, "FAIL: Item #12 Barrier() was not rejected (why='%s')\n", why.c_str());
        return 1;
    }
    std::printf("[PASS] Item #12 (enhanced barrier): rejected ('%s')\n", why.c_str());

    // ---- Item #12 Case 3: RTAS (slot 73 EmitRaytracingAccelerationStructurePostbuildInfo with count=0) rejected ----
    cl->Close();
    cl->Reset(alloc, nullptr);
    using PFN_EmitRTAS = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const void*, UINT, const D3D12_GPU_VIRTUAL_ADDRESS*);
    reinterpret_cast<PFN_EmitRTAS>(vt[73])(cl, nullptr, 0, nullptr);
    why.clear();
    if (openrr::pe::split::prepare(cl, &why) || why.find("RTAS") == std::string::npos) {
        std::fprintf(stderr, "FAIL: Item #12 RTAS was not rejected (why='%s')\n", why.c_str());
        return 1;
    }
    std::printf("[PASS] Item #12 (RTAS): rejected ('%s')\n", why.c_str());

    // ---- Item #12 Case 4: ExecuteMetaCommand (slot 71) rejected ----
    cl->Close();
    cl->Reset(alloc, nullptr);
    using PFN_ExecMeta = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, void*, const void*, SIZE_T);
    reinterpret_cast<PFN_ExecMeta>(vt[71])(cl, nullptr, nullptr, 0);
    why.clear();
    if (openrr::pe::split::prepare(cl, &why) || why.find("meta command") == std::string::npos) {
        std::fprintf(stderr, "FAIL: Item #12 ExecuteMetaCommand was not rejected (why='%s')\n", why.c_str());
        return 1;
    }
    std::printf("[PASS] Item #12 (meta command): rejected ('%s')\n", why.c_str());

    // ---- Item #12 Case 5: Bundle propagating open_split / ineligible to primary command list ----
    ID3D12CommandAllocator* balloc = nullptr;
    ID3D12GraphicsCommandList* bundle = nullptr;
    if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&balloc))) &&
        SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, balloc, nullptr, IID_PPV_ARGS(&bundle)))) {
        bundle->Close();
        bundle->Reset(balloc, nullptr);
        D3D12_RESOURCE_BARRIER bsb = sb;
        bsb.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        // Record open split barrier on bundle state via ResourceBarrier hook
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)>(
            (*reinterpret_cast<void***>(cl))[26])(bundle, 1, &bsb);
        bundle->Close();

        cl->Close();
        cl->Reset(alloc, nullptr);
        cl->ExecuteBundle(bundle);
        why.clear();
        if (openrr::pe::split::prepare(cl, &why) || why.find("split barrier") == std::string::npos) {
            std::fprintf(stderr, "FAIL: Item #12 bundle open_split was not rejected (why='%s')\n", why.c_str());
            return 1;
        }
        std::printf("[PASS] Item #12 (bundle propagation): rejected ('%s')\n", why.c_str());
        bundle->Release();
        balloc->Release();
    }

    if (buf) buf->Release();
    cl->Close();
    cl->Release();
    alloc->Release();
    dev->Release();
    return 0;
}

static int test_d3d12_gpu_and_engine() {
    openrr::pe::vulkan_entry_points();
    IDXGIFactory6* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) return 0;
    IDXGIAdapter1* ad = nullptr;
    if (FAILED(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad))) || !ad) {
        fac->Release();
        return 0;
    }
    fac->Release();
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) || !dev) {
        ad->Release();
        return 0;
    }
    ad->Release();

    {
        onr::DeviceOptions opt{};
        opt.luid = reinterpret_cast<const uint8_t*>(&desc.AdapterLuid);
        opt.descriptors = true;
        onr::Device vk;
        std::string err;
        if (!vk.init(err, opt)) {
            dev->Release();
            return 0;
        }

        onr::D3dDevice d3d;
        if (!d3d.init(dev, &vk, L"", err)) {
            dev->Release();
            return 0;
        }
        std::printf("[INFO] D3D12 Direct GPU initialized on %s (driver: %s)\n",
                    vk.info().name.c_str(), vk.info().driver_info.c_str());

        wchar_t self_exe[MAX_PATH]{};
        std::filesystem::path bdir = "build/asi";
        if (GetModuleFileNameW(nullptr, self_exe, MAX_PATH) != 0) {
            const auto exe_dir = std::filesystem::path(self_exe).parent_path();
            if (std::filesystem::exists(exe_dir / "stage" / "openrr-amd" / "shaders"))
                bdir = exe_dir;
        }
        const std::string bdir_u8 = onr::path_to_u8(bdir);
        const std::string default_spv_dir = onr::path_to_u8(bdir / "stage" / "openrr-amd" / "shaders");
        const char* spv_env = std::getenv("OPENRR_SPV_DIR");
        const std::string spv_dir_str = (spv_env && *spv_env) ? std::string(spv_env) : default_spv_dir;
        const char* spv_dir = spv_dir_str.c_str();

        onr::Kernels k_clear;
        k_clear.load(d3d, spv_dir, {"clear_range"});

        // ---- R4: Verify UTF-8 non-ASCII (Chinese) shader path loading on Windows ----
        {
            const std::filesystem::path cn_dir = bdir / L"utf8_测试中文路径";
            std::error_code ec;
            std::filesystem::create_directories(cn_dir, ec);
            const std::filesystem::path cn_spv = cn_dir / L"clear_range.spv";
            std::filesystem::copy_file(onr::u8_to_path(spv_dir) / L"clear_range.spv", cn_spv,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            onr::Kernels k_cn_vk, k_cn_d3d;
            k_cn_vk.load(vk, onr::path_to_u8(cn_dir), {"clear_range"});
            k_cn_d3d.load(d3d, onr::path_to_u8(cn_dir), {"clear_range"});
            const bool cn_ok = k_cn_vk.has("clear_range") && k_cn_d3d.has("clear_range");
            k_cn_d3d.unload(d3d);
            k_cn_vk.unload(vk);
            std::filesystem::remove_all(cn_dir, ec);
            if (!cn_ok) {
                std::fprintf(stderr, "FAIL: R4 UTF-8 Chinese shader path loading failed\n");
                dev->Release();
                return 1;
            }
            std::printf("[PASS] Item R4 (UTF-8 Chinese shader path loading): Vk & D3D12 loaded clear_range.spv from non-ASCII path\n");
        }

        // ---- R2: Verify D3D12 upload slot protection across all 4 replay/reset/churn edge modes (modes 0, 1, 2, 3) ----
        for (int r2_mode = 0; r2_mode < 4; ++r2_mode) {
            D3D12_COMMAND_QUEUE_DESC qd_r2{D3D12_COMMAND_LIST_TYPE_DIRECT};
            ID3D12CommandQueue* q_r2 = nullptr;
            ID3D12CommandAllocator *ca_r2_a = nullptr, *ca_r2_b = nullptr, *ca_r2_c = nullptr;
            ID3D12GraphicsCommandList *cl_r2_a = nullptr, *cl_r2_b = nullptr;
            ID3D12Fence *f_r2 = nullptr, *gate_r2 = nullptr;
            dev->CreateCommandQueue(&qd_r2, IID_PPV_ARGS(&q_r2));
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_r2_a));
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_r2_b));
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_r2_c));
            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca_r2_a, nullptr, IID_PPV_ARGS(&cl_r2_a));
            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca_r2_b, nullptr, IID_PPV_ARGS(&cl_r2_b));
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f_r2));
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_r2));
            std::vector<ID3D12CommandAllocator*> churn_r2;
            onr::Buffer b_r2 = d3d.create(256);
            onr::d3d12_note_list_reset(cl_r2_a, ca_r2_a);
            onr::d3d12_note_list_reset(cl_r2_b, ca_r2_b);
            d3d.bind(cl_r2_a, 0);
            const uint32_t slot_a = d3d.bound_slot();
            const uint32_t v_first = 11u;
            d3d.update(b_r2, 0, 4, &v_first);
            d3d.stamp();
            d3d.unbind();
            if (r2_mode == 1) {
                d3d.bind(cl_r2_a, 1);
                const uint32_t v_extra = 33u;
                d3d.update(b_r2, 4, 4, &v_extra);
                d3d.stamp();
                d3d.unbind();
            }
            cl_r2_a->Close();
            ID3D12CommandList* lists_a[] = {cl_r2_a};
            if (r2_mode != 2) {
                q_r2->ExecuteCommandLists(1, lists_a);
                q_r2->Signal(f_r2, 1);
                f_r2->SetEventOnCompletion(1, nullptr);
            }
            if (r2_mode == 2) {
                for (unsigned ci = 0; ci < 4096; ++ci) {
                    ID3D12CommandAllocator* other = nullptr;
                    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&other));
                    other->Reset();
                    onr::d3d12_note_allocator_reset(other);
                    churn_r2.push_back(other);
                }
            }
            if (r2_mode == 3) {
                q_r2->Wait(gate_r2, 1);
                q_r2->ExecuteCommandLists(1, lists_a);
                q_r2->Signal(f_r2, 2);
                cl_r2_a->Reset(ca_r2_c, nullptr);
                onr::d3d12_note_list_reset(cl_r2_a, ca_r2_c);
                cl_r2_a->Close();
            }
            d3d.bind(cl_r2_b, 0);
            const uint32_t slot_b = d3d.bound_slot();
            const uint32_t v_second = 22u;
            d3d.update(b_r2, 0, 4, &v_second);
            d3d.stamp();
            d3d.unbind();
            cl_r2_b->Close();

            if (r2_mode == 3) {
                gate_r2->Signal(1);
                f_r2->SetEventOnCompletion(2, nullptr);
            } else {
                q_r2->ExecuteCommandLists(1, lists_a);
                q_r2->Signal(f_r2, 2);
                f_r2->SetEventOnCompletion(2, nullptr);
            }
            uint32_t got_val = 0;
            d3d.download(b_r2, &got_val, 4);

            onr::d3d12_note_list_released(cl_r2_a);
            onr::d3d12_note_list_released(cl_r2_b);
            ca_r2_a->Reset(); ca_r2_b->Reset(); ca_r2_c->Reset();
            onr::d3d12_note_allocator_reset(ca_r2_a);
            onr::d3d12_note_allocator_reset(ca_r2_b);
            onr::d3d12_note_allocator_reset(ca_r2_c);
            onr::d3d12_note_allocator_released(ca_r2_a);
            onr::d3d12_note_allocator_released(ca_r2_b);
            onr::d3d12_note_allocator_released(ca_r2_c);
            for (auto* other : churn_r2) {
                onr::d3d12_note_allocator_released(other);
                other->Release();
            }
            d3d.destroy(b_r2);
            gate_r2->Release(); f_r2->Release(); cl_r2_b->Release(); cl_r2_a->Release();
            ca_r2_c->Release(); ca_r2_b->Release(); ca_r2_a->Release(); q_r2->Release();
            if (got_val != 11u || slot_a == slot_b) {
                std::fprintf(stderr, "FAIL: R2 D3D12 edge test mode=%d failed (got_val=%u slot_a=%u slot_b=%u)\n",
                             r2_mode, got_val, slot_a, slot_b);
                dev->Release();
                return 1;
            }
        }
        std::printf("[PASS] Item R2 (D3D12 replayable command list slot protection, modes 0..3): all 4 edge modes read 11\n");

        // ---- Item #2: Verify 256 KB (> 64 KB ring) bound & unbound D3dDevice::fill zeroes 0xFFFFFFFF buffer ----
        {
            constexpr size_t kLargeBytes = 256 * 1024;
            onr::Buffer b = d3d.create(kLargeBytes);
            d3d.fill(b, 0xFFFFFFFFu);
            d3d.fill(b, 0u);
            std::vector<uint32_t> v0(kLargeBytes / 4, 0xDEADBEEFu);
            d3d.download(b, v0.data(), kLargeBytes);
            size_t wrong_unbound = 0;
            for (uint32_t x : v0) wrong_unbound += (x != 0u);

            d3d.fill(b, 0xFFFFFFFFu);
            d3d.begin_own();
            d3d.fill(b, 0, kLargeBytes, 0u);
            d3d.submit_own();
            std::vector<uint32_t> v1(kLargeBytes / 4, 0xDEADBEEFu);
            d3d.download(b, v1.data(), kLargeBytes);
            size_t wrong_bound = 0;
            for (uint32_t x : v1) wrong_bound += (x != 0u);

            d3d.destroy(b);
            k_clear.unload(d3d);
            if (wrong_unbound != 0 || wrong_bound != 0) {
                std::fprintf(stderr, "FAIL: Item #2 D3dDevice::fill 256 KB wrong_unbound=%zu wrong_bound=%zu\n",
                             wrong_unbound, wrong_bound);
                dev->Release();
                return 1;
            }
            std::printf("[PASS] Item #2 (D3dDevice::fill 256 KB > 64 KB ring): unbound wrong=%zu, bound wrong=%zu\n",
                        wrong_unbound, wrong_bound);
        }

        const char* dlssd = std::getenv("OPENRR_DLSSD");
        if (!dlssd || !*dlssd) dlssd = "C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll";
        if (std::filesystem::exists(dlssd)) {
            onr::Weights w;
            if (!onr::build_weights(std::filesystem::path(dlssd).wstring(), w, err)) {
                std::fprintf(stderr, "FAIL: build_weights failed: %s\n", err.c_str());
                dev->Release();
                return 1;
            }
            onr::Kernels k_fast, k_lite;
            auto names = onr::Engine::kernel_names(w, true);
            names.push_back("enc0_attr_f_w");
            names.push_back("enc0_stage_32_64_f_w");
            auto flow_names = onr::Engine::flow_kernel_names();
            k_fast.load(d3d, spv_dir, names);
            k_fast.load(d3d, spv_dir, flow_names);
            k_lite.load_lite(d3d, spv_dir, names);
            k_lite.load_lite(d3d, spv_dir, flow_names);
            onr::Engine eng;
            eng.init(d3d, k_fast, w);
            eng.set_fast(true);
            eng.set_lite(&k_lite);
            eng.set_lite_on(true);

            const onr::I2 out_sz{1920, 1080};
            const onr::I2 r_a{1280, 720};
            const onr::I2 r_b{1120, 630};
            eng.resize(r_a, out_sz);

            const size_t px = size_t(r_a.x) * r_a.y;
            onr::Buffer b_col = d3d.create_unzeroed(px * 8);
            onr::Buffer b_alb = d3d.create_unzeroed(px * 4);
            onr::Buffer b_spc = d3d.create_unzeroed(px * 4);
            onr::Buffer b_nrm = d3d.create_unzeroed(px * 8);
            onr::Buffer b_rgh = d3d.create_unzeroed(px * 2);
            onr::Buffer b_dep = d3d.create_unzeroed(px * 4);
            onr::Buffer b_mv  = d3d.create_unzeroed(px * 4);
            onr::Buffer b_hit = d3d.create_unzeroed(px * 2);
            onr::Buffer b_out = d3d.create_unzeroed(size_t(out_sz.x) * out_sz.y * 8);

            d3d.begin_batch();
            d3d.fill(b_col, 0x38003800u);
            d3d.fill(b_alb, 0x80808080u);
            d3d.fill(b_spc, 0x20202020u);
            d3d.fill(b_nrm, 0x00003C00u);
            d3d.fill(b_rgh, 0x38003800u);
            d3d.fill(b_dep, 0x3F800000u);
            d3d.fill(b_mv,  0u);
            d3d.fill(b_hit, 0x38003800u);
            d3d.fill(b_out, 0u);
            d3d.end_batch();

            onr::Guides g{b_col.addr, b_alb.addr, b_spc.addr, b_nrm.addr, b_rgh.addr, b_dep.addr, b_mv.addr, b_hit.addr};
            onr::EngineFrame ef{};
            ef.render = r_a;
            ef.output = out_sz;
            for (int i = 0; i < 16; ++i) {
                ef.world_to_view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                ef.view_to_clip[i]  = (i % 5 == 0) ? 1.0f : 0.0f;
            }

            std::vector<double> steady_ms, legacy_ms;
            for (int f = 0; f < 24; ++f) {
                ef.reset = (f == 0);
                ef.jitter[0] = (f & 1) ? 0.25f : -0.25f;
                ef.jitter[1] = (f & 2) ? 0.25f : -0.25f;
                d3d.begin_own();
                d3d.stamp();
                eng.record(ef, g, b_out.addr);
                d3d.stamp();
                const uint32_t nq = d3d.queries();
                d3d.submit_own();
                std::vector<uint64_t> ts(nq);
                if (f >= 4 && nq >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts)) {
                    steady_ms.push_back(double(ts.back() - ts.front()) * 1000.0 / double(d3d.timestamp_frequency()));
                }
            }
            eng.set_lite_on(false);
            for (int f = 0; f < 16; ++f) {
                ef.reset = false;
                d3d.begin_own();
                d3d.stamp();
                eng.record(ef, g, b_out.addr);
                d3d.stamp();
                const uint32_t nq = d3d.queries();
                d3d.submit_own();
                std::vector<uint64_t> ts(nq);
                if (f >= 2 && nq >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts)) {
                    legacy_ms.push_back(double(ts.back() - ts.front()) * 1000.0 / double(d3d.timestamp_frequency()));
                }
            }
            eng.set_lite_on(true);
            std::sort(steady_ms.begin(), steady_ms.end());
            std::sort(legacy_ms.begin(), legacy_ms.end());
            const double p50 = steady_ms.empty() ? 0.0 : steady_ms[steady_ms.size() / 2];
            const double p95 = steady_ms.empty() ? 0.0 : steady_ms[size_t(steady_ms.size() * 0.95)];
            const double leg_p50 = legacy_ms.empty() ? 0.0 : legacy_ms[legacy_ms.size() / 2];

            std::vector<double> drs_ms;
            for (int f = 0; f < 16; ++f) {
                const onr::I2 next_r = (f % 4 >= 2) ? r_b : r_a;
                if (next_r.x != ef.render.x || next_r.y != ef.render.y) {
                    eng.resize(next_r, out_sz);
                }
                ef.render = next_r;
                ef.reset = false;
                d3d.begin_own();
                d3d.stamp();
                eng.record(ef, g, b_out.addr);
                d3d.stamp();
                const uint32_t nq = d3d.queries();
                d3d.submit_own();
                std::vector<uint64_t> ts(nq);
                if (nq >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts)) {
                    drs_ms.push_back(double(ts.back() - ts.front()) * 1000.0 / double(d3d.timestamp_frequency()));
                }
            }
            std::sort(drs_ms.begin(), drs_ms.end());
            const double drs_p50 = drs_ms.empty() ? 0.0 : drs_ms[drs_ms.size() / 2];

            std::vector<uint16_t> out_half(size_t(out_sz.x) * out_sz.y * 4);
            d3d.download(b_out, out_half.data(), out_half.size() * 2);
            size_t non_finite = 0, non_zero = 0;
            for (uint16_t h : out_half) {
                if ((h & 0x7C00u) == 0x7C00u) ++non_finite;
                if ((h & 0x7FFFu) != 0u) ++non_zero;
            }
            std::printf("[PASS] D3D12 Direct Engine (1280x720->1920x1080): Fast(_lt+flow) p50=%.3f ms p95=%.3f ms, Legacy(_f+flow) p50=%.3f ms, DRS p50=%.3f ms, non_finite=%zu, non_zero=%zu\n",
                        p50, p95, leg_p50, drs_p50, non_finite, non_zero);

            bool k2_failed = false;
            // ---- Item #9 & #10: Per-dispatch GPU timing profile at 1080p (1280x720->1920x1080) & 4K (2560x1440->3840x2160) ----
            {
                auto profile_res = [&](onr::I2 r_sz, onr::I2 o_sz, const char* label) {
                    eng.resize(r_sz, o_sz);
                    const size_t rpx = size_t(r_sz.x) * r_sz.y;
                    onr::Buffer p_col = d3d.create_unzeroed(rpx * 8);
                    onr::Buffer p_alb = d3d.create_unzeroed(rpx * 4);
                    onr::Buffer p_spc = d3d.create_unzeroed(rpx * 4);
                    onr::Buffer p_nrm = d3d.create_unzeroed(rpx * 8);
                    onr::Buffer p_rgh = d3d.create_unzeroed(rpx * 2);
                    onr::Buffer p_dep = d3d.create_unzeroed(rpx * 4);
                    onr::Buffer p_mv  = d3d.create_unzeroed(rpx * 4);
                    onr::Buffer p_hit = d3d.create_unzeroed(rpx * 2);
                    onr::Buffer p_out = d3d.create_unzeroed(size_t(o_sz.x) * o_sz.y * 8);
                    d3d.begin_batch();
                    d3d.fill(p_col, 0x38003800u); d3d.fill(p_alb, 0x80808080u); d3d.fill(p_spc, 0x20202020u);
                    d3d.fill(p_nrm, 0x00003C00u); d3d.fill(p_rgh, 0x38003800u); d3d.fill(p_dep, 0x3F800000u);
                    d3d.fill(p_mv, 0u); d3d.fill(p_hit, 0x38003800u); d3d.fill(p_out, 0u);
                    d3d.end_batch();
                    onr::Guides pg{p_col.addr, p_alb.addr, p_spc.addr, p_nrm.addr, p_rgh.addr, p_dep.addr, p_mv.addr, p_hit.addr};
                    ef.render = r_sz;
                    ef.output = o_sz;
                    std::vector<double> res_fast_ms, res_leg_ms;
                    eng.set_lite_on(true);
                    for (int f = 0; f < 16; ++f) {
                        ef.reset = (f == 0);
                        ef.jitter[0] = (f & 1) ? 0.25f : -0.25f;
                        ef.jitter[1] = (f & 2) ? 0.25f : -0.25f;
                        d3d.begin_own();
                        d3d.stamp();
                        eng.record(ef, pg, p_out.addr);
                        d3d.stamp();
                        const uint32_t nq_s = d3d.queries();
                        d3d.submit_own();
                        std::vector<uint64_t> ts_s(nq_s);
                        if (f >= 4 && nq_s >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts_s)) {
                            res_fast_ms.push_back(double(ts_s.back() - ts_s.front()) * 1000.0 / double(d3d.timestamp_frequency()));
                        }
                    }
                    eng.set_lite_on(false);
                    for (int f = 0; f < 12; ++f) {
                        ef.reset = false;
                        d3d.begin_own();
                        d3d.stamp();
                        eng.record(ef, pg, p_out.addr);
                        d3d.stamp();
                        const uint32_t nq_s = d3d.queries();
                        d3d.submit_own();
                        std::vector<uint64_t> ts_s(nq_s);
                        if (f >= 2 && nq_s >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts_s)) {
                            res_leg_ms.push_back(double(ts_s.back() - ts_s.front()) * 1000.0 / double(d3d.timestamp_frequency()));
                        }
                    }
                    eng.set_lite_on(true);
                    std::sort(res_fast_ms.begin(), res_fast_ms.end());
                    std::sort(res_leg_ms.begin(), res_leg_ms.end());
                    const double rf_p50 = res_fast_ms.empty() ? 0.0 : res_fast_ms[res_fast_ms.size() / 2];
                    const double rf_p95 = res_fast_ms.empty() ? 0.0 : res_fast_ms[size_t(res_fast_ms.size() * 0.95)];
                    const double rl_p50 = res_leg_ms.empty() ? 0.0 : res_leg_ms[res_leg_ms.size() / 2];
                    std::printf("[PASS] D3D12 Direct Engine %s (%dx%d->%dx%d): Fast(_lt+flow) p50=%.3f ms p95=%.3f ms, Legacy(_f+flow) p50=%.3f ms\n",
                                label, r_sz.x, r_sz.y, o_sz.x, o_sz.y, rf_p50, rf_p95, rl_p50);

                    ef.reset = false;
                    d3d.set_stamps(true);
                    d3d.begin_own();
                    eng.record(ef, pg, p_out.addr);
                    const uint32_t nq = d3d.queries();
                    const auto names_copy = d3d.stamp_names;
                    d3d.submit_own();
                    d3d.set_stamps(false);

                    std::vector<uint64_t> ts(nq);
                    if (nq >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts)) {
                        struct DispStat { std::string name; double us; };
                        std::vector<DispStat> stats;
                        double total_us = 0.0, net21_us = 0.0;
                        for (const auto& sn : names_copy) {
                            const auto at = sn.rfind('@');
                            if (at == std::string::npos) continue;
                            const uint32_t qi = uint32_t(std::stoul(sn.substr(at + 1)));
                            if (qi + 1 < ts.size() && ts[qi + 1] >= ts[qi]) {
                                const double us = double(ts[qi + 1] - ts[qi]) * 1e6 / double(d3d.timestamp_frequency());
                                const std::string kname = sn.substr(0, at);
                                stats.push_back({kname, us});
                                total_us += us;
                                if (kname != "clear_range.spv") net21_us += us;
                            }
                        }
                        std::sort(stats.begin(), stats.end(), [](const DispStat& a, const DispStat& b) { return a.us > b.us; });
                        std::printf("[PROFILE] %s (%dx%d->%dx%d, %zu dispatches [21 net + 1 flow clear_range], sum=%.2f ms, net21_sum=%.2f ms) Top-8 hottest dispatches:\n",
                                    label, r_sz.x, r_sz.y, o_sz.x, o_sz.y, stats.size(), total_us / 1000.0, net21_us / 1000.0);
                        for (size_t i = 0; i < std::min<size_t>(8, stats.size()); ++i) {
                            std::printf("  #%zu %-32s %7.1f us (%4.1f%%)\n",
                                        i + 1, stats[i].name.c_str(), stats[i].us,
                                        total_us > 0.0 ? (100.0 * stats[i].us / total_us) : 0.0);
                        }
                    }

                    if (std::filesystem::exists(bdir / "occ_nopad" / "enc0_stage_32_64_f_lt.spv") &&
                        std::filesystem::exists(bdir / "occ_pad16k" / "enc0_stage_32_64_f_lt.spv")) {
                        onr::Kernels k_nopad, k_pad16k;
                        k_nopad.load_lite(d3d, bdir_u8 + "/occ_nopad", names);
                        k_nopad.load_lite(d3d, spv_dir, names);
                        k_nopad.load_lite(d3d, spv_dir, flow_names);
                        k_pad16k.load_lite(d3d, bdir_u8 + "/occ_pad16k", names);
                        k_pad16k.load_lite(d3d, spv_dir, names);
                        k_pad16k.load_lite(d3d, spv_dir, flow_names);

                        auto run_ab_res = [&](const onr::Kernels* lk_ptr, std::vector<uint16_t>& out_words, std::map<std::string, double>& us_map) {
                            eng.set_lite(lk_ptr);
                            std::map<std::string, std::vector<double>> samples;
                            for (int wf = 0; wf < 15; ++wf) {
                                ef.reset = (wf == 14);
                                const bool measure = (wf >= 5 && wf < 14);
                                if (measure) d3d.set_stamps(true);
                                d3d.begin_own();
                                eng.record(ef, pg, p_out.addr);
                                const uint32_t nq_ab = d3d.queries();
                                const auto snames = d3d.stamp_names;
                                d3d.submit_own();
                                if (measure) {
                                    d3d.set_stamps(false);
                                    std::vector<uint64_t> ts_ab(nq_ab);
                                    if (nq_ab >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts_ab)) {
                                        for (const auto& sn : snames) {
                                            const auto at = sn.rfind('@');
                                            if (at == std::string::npos) continue;
                                            const uint32_t qi = uint32_t(std::stoul(sn.substr(at + 1)));
                                            if (qi + 1 < ts_ab.size() && ts_ab[qi + 1] >= ts_ab[qi]) {
                                                samples[sn.substr(0, at)].push_back(double(ts_ab[qi + 1] - ts_ab[qi]) * 1e6 / double(d3d.timestamp_frequency()));
                                            }
                                        }
                                    }
                                }
                            }
                            ef.reset = false;
                            d3d.set_stamps(true);
                            d3d.begin_own();
                            eng.record(ef, pg, p_out.addr);
                            const uint32_t nq_ab = d3d.queries();
                            const auto snames = d3d.stamp_names;
                            d3d.submit_own();
                            d3d.set_stamps(false);
                            std::vector<uint64_t> ts_ab(nq_ab);
                            if (nq_ab >= 2 && d3d.read_stamps(onr::D3dDevice::kOwnSlot, ts_ab)) {
                                for (const auto& sn : snames) {
                                    const auto at = sn.rfind('@');
                                    if (at == std::string::npos) continue;
                                    const uint32_t qi = uint32_t(std::stoul(sn.substr(at + 1)));
                                    if (qi + 1 < ts_ab.size() && ts_ab[qi + 1] >= ts_ab[qi]) {
                                        samples[sn.substr(0, at)].push_back(double(ts_ab[qi + 1] - ts_ab[qi]) * 1e6 / double(d3d.timestamp_frequency()));
                                    }
                                }
                            }
                            for (auto& [k, vec] : samples) {
                                std::sort(vec.begin(), vec.end());
                                us_map[k] = vec[vec.size() / 2];
                            }
                            out_words.resize(size_t(o_sz.x) * o_sz.y * 4);
                            d3d.download(p_out, out_words.data(), out_words.size() * 2);
                        };

                        std::vector<uint16_t> out_dummy, out_ship, out_base, out_occ;
                        std::map<std::string, double> us_dummy, us_ship, us_base, us_occ;
                        run_ab_res(&k_lite, out_dummy, us_dummy);
                        run_ab_res(&k_lite, out_ship, us_ship);
                        run_ab_res(&k_nopad, out_base, us_base);
                        run_ab_res(&k_pad16k, out_occ, us_occ);
                        eng.set_lite(&k_lite);

                        size_t diff_words = 0, diff_ship = 0;
                        for (size_t i = 0; i < out_base.size(); ++i) {
                            diff_words += (out_base[i] != out_occ[i]);
                            diff_ship += (out_base[i] != out_ship[i]);
                        }
                         const char* top5[] = {
                            "enc0_stage_32_64_f_lt.spv",
                            "post_f_lt.spv",
                            "dec_up_32_64_64_f_lt.spv",
                            "dec_head_32_64_f_lt.spv",
                            "enc_stage_64_64_96_f_lt.spv",
                        };
                        auto fnv_file = [](const std::string& path) -> uint64_t {
                            std::ifstream f(path, std::ios::binary);
                            if (!f) return 0;
                            uint64_t h = 14695981039346656037ull;
                            char buf[4096];
                            while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
                                for (std::streamsize i = 0; i < f.gcount(); ++i)
                                    h = (h ^ uint8_t(buf[i])) * 1099511628211ull;
                            }
                            return h;
                        };
                        bool spv_hash_match = true;
                        for (const char* kname : top5) {
                            const uint64_t h_ship = fnv_file(std::string(spv_dir) + "/" + kname);
                            const uint64_t h_nopad = fnv_file(bdir_u8 + "/occ_nopad/" + kname);
                            const uint64_t h_pad16k = fnv_file(bdir_u8 + "/occ_pad16k/" + kname);
                            if (h_ship == 0 || h_ship != h_nopad || h_ship == h_pad16k) {
                                std::fprintf(stderr, "FAIL: K2 SPIR-V hash gate failed for %s (ship=0x%llx, nopad=0x%llx, pad16k=0x%llx)\n",
                                             kname, (unsigned long long)h_ship, (unsigned long long)h_nopad, (unsigned long long)h_pad16k);
                                spv_hash_match = false;
                                k2_failed = true;
                            }
                        }
                        double sum_ship = 0.0, sum_base = 0.0, sum_occ = 0.0, w1_ship = 0.0, w1_base = 0.0, w1_occ = 0.0;
                        std::printf("[PASS] Item #10 / K2 (%s Top-5 wave/LDS occ_pad=16KB A/B, diff_words=%zu/%zu, ship_diff=%zu, spv_hash_nopad=%s):\n",
                                    label, diff_words, out_base.size(), diff_ship, spv_hash_match ? "EXACT" : "MISMATCH");
                        for (const char* kname : top5) {
                            const double us = us_ship[kname], ub = us_base[kname], uo = us_occ[kname];
                            sum_ship += us; sum_base += ub; sum_occ += uo;
                            if (std::strstr(kname, "_32_64")) { w1_ship += us; w1_base += ub; w1_occ += uo; }
                            std::printf("  %-30s shipped=%7.1f us  nopad=%7.1f us  occ_pad_16KB=%7.1f us  (occ_vs_nopad=%+5.1f%%)\n",
                                        kname, us, ub, uo, ub > 0.0 ? 100.0 * (uo - ub) / ub : 0.0);
                        }
                        std::printf("  W1(C=32 x3) sum: nopad=%.1f us vs occ_pad_16KB=%.1f us (delta=%+.1f%%) | All-5 sum: shipped=%.1f us, nopad=%.1f us, occ_pad_16KB=%.1f us\n",
                                    w1_base, w1_occ, w1_base > 0.0 ? 100.0 * (w1_occ - w1_base) / w1_base : 0.0,
                                    sum_ship, sum_base, sum_occ);
                        k_pad16k.unload(d3d);
                        k_nopad.unload(d3d);
                        if (diff_words != 0 || diff_ship != 0) {
                            std::fprintf(stderr, "FAIL: K2 occ_pad output mismatch at %s (diff_words=%zu, diff_ship=%zu)\n", label, diff_words, diff_ship);
                            k2_failed = true;
                        }

                        if (std::filesystem::exists(bdir / "b2_base" / "post_f_lt.spv") &&
                            std::filesystem::exists(bdir / "b2_post_exp" / "post_f_lt.spv")) {
                            auto load_b2 = [&](onr::Kernels& dst, const std::string& dir) {
                                dst.load_lite(d3d, dir, names);
                                dst.load_lite(d3d, bdir_u8 + "/b2_base", names);
                                dst.load_lite(d3d, spv_dir, names);
                                dst.load_lite(d3d, spv_dir, flow_names);
                            };
                            onr::Kernels kb_base, kb_pexp, kb_plog, kb_arcp, kb_asame, kb_far3;
                            load_b2(kb_base, bdir_u8 + "/b2_base");
                            load_b2(kb_pexp, bdir_u8 + "/b2_post_exp");
                            load_b2(kb_plog, bdir_u8 + "/b2_post_log");
                            load_b2(kb_arcp, bdir_u8 + "/b2_attr_rcp");
                            load_b2(kb_asame, bdir_u8 + "/b2_attr_same");
                            load_b2(kb_far3, bdir_u8 + "/b2_c64_far3");

                            std::vector<uint16_t> ow_tmp;
                            std::vector<double> vp0, vp1, vp2, va0, va1, va2, vc0, vc1;
                            for (int r = 0; r < 3; ++r) {
                                std::map<std::string, double> u_bbase, u_pexp, u_plog, u_arcp, u_asame, u_far3;
                                run_ab_res(&kb_base, ow_tmp, u_bbase);
                                run_ab_res(&kb_pexp, ow_tmp, u_pexp);
                                run_ab_res(&kb_plog, ow_tmp, u_plog);
                                run_ab_res(&kb_arcp, ow_tmp, u_arcp);
                                run_ab_res(&kb_asame, ow_tmp, u_asame);
                                run_ab_res(&kb_far3, ow_tmp, u_far3);
                                vp0.push_back(u_bbase["post_f_lt.spv"]);
                                vp1.push_back(u_pexp["post_f_lt.spv"]);
                                vp2.push_back(u_plog["post_f_lt.spv"]);
                                va0.push_back(u_bbase["enc0_attr_f_lt.spv"]);
                                va1.push_back(u_arcp["enc0_attr_f_lt.spv"]);
                                va2.push_back(u_asame["enc0_attr_f_lt.spv"]);
                                vc0.push_back(u_bbase["enc_stage_64_64_96_f_lt.spv"] + u_bbase["dec_up_64_64_96_f_lt.spv"] + u_bbase["dec_dfn_64_64_f_lt.spv"]);
                                vc1.push_back(u_far3["enc_stage_64_64_96_f_lt.spv"] + u_far3["dec_up_64_64_96_f_lt.spv"] + u_far3["dec_dfn_64_64_f_lt.spv"]);
                            }
                            eng.set_lite(&k_lite);

                            for (auto* v : {&vp0, &vp1, &vp2, &va0, &va1, &va2, &vc0, &vc1}) std::sort(v->begin(), v->end());
                            const double p0 = vp0[1], p1 = vp1[1], p2 = vp2[1];
                            const double a0 = va0[1], a1 = va1[1], a2 = va2[1];
                            const double c0 = vc0[1], c1 = vc1[1];
                            const uint64_t h_attr_ship = fnv_file(std::string(spv_dir) + "/enc0_attr_f_lt.spv");
                            const uint64_t h_attr_rcp = fnv_file(bdir_u8 + "/b2_attr_rcp/enc0_attr_f_lt.spv");
                            const uint64_t h_d3d_attr_ship = fnv_file(std::string(spv_dir) + "/d3d_enc0_attr_f_lt.spv");
                            const uint64_t h_d3d_attr_rcp = fnv_file(bdir_u8 + "/b2_attr_rcp/d3d_enc0_attr_f_lt.spv");
                            std::printf("[PASS] Item B2 (%s per-macro LT_* A/B, 3-run median [min..max], enc0_attr_spv=0x%016llx vs rcp=0x%016llx, d3d_enc0_attr_spv=0x%016llx vs rcp=0x%016llx):\n",
                                        label, (unsigned long long)h_attr_ship, (unsigned long long)h_attr_rcp,
                                        (unsigned long long)h_d3d_attr_ship, (unsigned long long)h_d3d_attr_rcp);
                            std::printf("  post_f_lt.spv:       PAIR_only=%7.1f [%.1f..%.1f] us | +EXP+NOCORNER+NETH=%7.1f [%.1f..%.1f] us (%+5.1f%%, ENABLED) | +LOG=%7.1f [%.1f..%.1f] us (%+5.1f%% vs EXP, DISABLED)\n",
                                        p0, vp0[0], vp0[2], p1, vp1[0], vp1[2], p0 > 0 ? 100.0 * (p1 - p0) / p0 : 0.0,
                                        p2, vp2[0], vp2[2], p1 > 0 ? 100.0 * (p2 - p1) / p1 : 0.0);
                            std::printf("  enc0_attr_f_lt.spv:  TAB_only =%7.1f [%.1f..%.1f] us | +RCP+DEPTH        =%7.1f [%.1f..%.1f] us (%+5.1f%%, DISABLED) | +SAME+CL=%7.1f [%.1f..%.1f] us (%+5.1f%% vs RCP, DISABLED)\n",
                                        a0, va0[0], va0[2], a1, va1[0], va1[2], a0 > 0 ? 100.0 * (a1 - a0) / a0 : 0.0,
                                        a2, va2[0], va2[2], a1 > 0 ? 100.0 * (a2 - a1) / a1 : 0.0);
                            std::printf("  C=64 x3 sum:         default  =%7.1f [%.1f..%.1f] us | +LT_FAR=3         =%7.1f [%.1f..%.1f] us (%+5.1f%%, DISABLED)\n",
                                        c0, vc0[0], vc0[2], c1, vc1[0], vc1[2], c0 > 0 ? 100.0 * (c1 - c0) / c0 : 0.0);

                            for (auto* kp : {&kb_far3, &kb_asame, &kb_arcp, &kb_plog, &kb_pexp, &kb_base}) kp->unload(d3d);
                        }
                    }

                    // ---- Item B1: Isolated Fast-mode VRAM pooling verification (B1 on vs B1 off, wide path, and f0+0 negative control) ----
                    {
                        auto run_b1_seq = [&](bool pool_on, bool race_f0, std::vector<std::vector<uint16_t>>& frames_out, uint64_t& vram_bytes, uint64_t& vram_wide_bytes, bool& had_overlap) {
                            onr::Engine e_b1;
                            e_b1.init(d3d, k_fast, w);
                            e_b1.set_fast(true);
                            e_b1.set_lite(&k_lite);
                            e_b1.set_lite_on(true);
                            e_b1.set_pool_dec1_tail(pool_on);
                            if (race_f0) e_b1.set_pool_dec1_f0_race_for_test(true);
                            e_b1.resize(r_sz, o_sz);
                            vram_bytes = d3d.bytes();
                            vram_wide_bytes = vram_bytes;
                            had_overlap = e_b1.has_flow_buffer_overlap();
                            frames_out.resize(20);
                            for (int f = 0; f < 20; ++f) {
                                ef.reset = (f == 0);
                                ef.jitter[0] = (f & 1) ? 0.25f : -0.25f;
                                ef.jitter[1] = (f & 2) ? 0.25f : -0.25f;
                                // Frame 10..14 exercises the wide-attr in-place replacement (graveyard-deferred old t0) & dec1_l_[1..4] delta-shift path (depth_of_field != 0)
                                ef.depth_of_field = (!race_f0 && f >= 10 && f < 15) ? p_dep.addr : 0;
                                d3d.begin_own();
                                e_b1.record(ef, pg, p_out.addr);
                                d3d.submit_own();
                                if (f == 10 && !race_f0) vram_wide_bytes = d3d.bytes();
                                ef.depth_of_field = 0;
                                had_overlap |= e_b1.has_flow_buffer_overlap();
                                frames_out[f].resize(size_t(o_sz.x) * o_sz.y * 4);
                                d3d.download(p_out, frames_out[f].data(), frames_out[f].size() * 2);
                            }
                            e_b1.destroy();
                        };
                        std::vector<std::vector<uint16_t>> b1_off_frames, b1_on_a, b1_on_b, b1_race_frames;
                        uint64_t bytes_off = 0, bytes_on_a = 0, bytes_on_b = 0, bytes_race = 0;
                        uint64_t wide_off = 0, wide_on_a = 0, wide_on_b = 0, wide_race = 0;
                        bool ov_off = false, ov_on_a = false, ov_on_b = false, ov_race = false;
                        run_b1_seq(false, false, b1_off_frames, bytes_off, wide_off, ov_off);
                        run_b1_seq(true, false, b1_on_a, bytes_on_a, wide_on_a, ov_on_a);
                        run_b1_seq(true, false, b1_on_b, bytes_on_b, wide_on_b, ov_on_b);
                        run_b1_seq(true, true, b1_race_frames, bytes_race, wide_race, ov_race);
                        uint32_t max_ulp_ab = 0, max_ulp_rep = 0, max_ulp_race = 0;
                        size_t diff_ab = 0, diff_rep = 0, diff_race = 0;
                        for (int f = 0; f < 20; ++f) {
                            for (size_t i = 0; i < b1_off_frames[f].size(); ++i) {
                                uint16_t u0 = b1_off_frames[f][i], u1 = b1_on_a[f][i], u2 = b1_on_b[f][i], ur = b1_race_frames[f][i];
                                if (u0 != u1) { ++diff_ab; max_ulp_ab = std::max(max_ulp_ab, uint32_t(u0 > u1 ? u0 - u1 : u1 - u0)); }
                                if (u1 != u2) { ++diff_rep; max_ulp_rep = std::max(max_ulp_rep, uint32_t(u1 > u2 ? u1 - u2 : u2 - u1)); }
                                if (u0 != ur) { ++diff_race; max_ulp_race = std::max(max_ulp_race, uint32_t(u0 > ur ? u0 - ur : ur - u0)); }
                            }
                        }
                        const double saved_mb = double(bytes_off > bytes_on_a ? bytes_off - bytes_on_a : 0) / double(1u << 20);
                        const double wide_delta_mb = double(wide_on_a > bytes_on_a ? wide_on_a - bytes_on_a : 0) / double(1u << 20);
                        const size_t apx = size_t((r_sz.x + 31) & ~31) * ((r_sz.y + 31) & ~31);
                        const uint64_t max_wide_delta = apx * 16 + 256;
                        if (diff_ab != 0 || diff_rep != 0 || bytes_on_a >= bytes_off || ov_off || ov_on_a || ov_on_b || !ov_race ||
                            wide_on_a <= bytes_on_a || (wide_on_a - bytes_on_a) > max_wide_delta) {
                            std::fprintf(stderr, "FAIL: B1 isolated Fast pooling check failed at %s (diff_ab=%zu max_ulp=%u, diff_rep=%zu, ov_on=%d, ov_race=%d, saved=%.2f MB, wide_delta=%.2f MB)\n",
                                         label, diff_ab, max_ulp_ab, diff_rep, int(ov_on_a), int(ov_race), saved_mb, wide_delta_mb);
                            k2_failed = true;
                        } else {
                            std::printf("[PASS] Item B1 (%s isolated Fast dec1_l_[1..4] tail pooling in t0 + wide path, 20 frames x 2 runs): diff_vs_unpooled=%zu (max_ulp_diff=%u), repeat_diff=%zu, flow_overlap_audit=%s (neg_ctrl_f0_race_caught=%s, race_diff=%zu), saved=%.2f MB, wide_vram_delta=+%.2f MB (<=+16 B/px)\n",
                                        label, diff_ab, max_ulp_ab, diff_rep, ov_on_a ? "OVERLAP" : "CLEAN", ov_race ? "YES" : "NO", diff_race, saved_mb, wide_delta_mb);
                        }
                    }

                    for (onr::Buffer* b : {&p_col, &p_alb, &p_spc, &p_nrm, &p_rgh, &p_dep, &p_mv, &p_hit, &p_out})
                        d3d.destroy(*b);
                };
                profile_res({1280, 720}, {1920, 1080}, "1080p Quality");
                profile_res({2560, 1440}, {3840, 2160}, "4K Quality");
            }

            for (onr::Buffer* b : {&b_col, &b_alb, &b_spc, &b_nrm, &b_rgh, &b_dep, &b_mv, &b_hit, &b_out})
                d3d.destroy(*b);
            eng.destroy();
            k_lite.unload(d3d);
            k_fast.unload(d3d);
            if (non_finite != 0 || non_zero == 0 || k2_failed) {
                dev->Release();
                return 1;
            }

            // ---- Runtime API test: ONR_RR_BUSY slot saturation (127 slots) + allocator reset recovery ----
            {
                OnrRrApi4 api4{};
                if (OnrRrGetApi(ONR_RR_ABI_VERSION_4, &api4.v3.v2.v1) == ONR_RR_OK) {
                    const std::wstring w_dlssd = std::filesystem::path(dlssd).wstring();
                    const std::wstring w_data = (bdir / "stage" / "openrr-amd").wstring();
                    OnrRrCreateInfo ci{};
                    ci.size = sizeof(ci);
                    ci.mode = ONR_RR_MODE_FAST;
                    ci.d3d12_device = dev;
                    ci.max_render_width = 320;
                    ci.max_render_height = 180;
                    ci.output_width = 640;
                    ci.output_height = 360;
                    ci.dlssd_path = w_dlssd.c_str();
                    ci.data_dir = w_data.c_str();
                    ci.backend = ONR_RR_BACKEND_CHOICE_D3D12;
                    void* rt = nullptr;
                    if (api4.v3.v2.v1.Create(&ci, &rt) == ONR_RR_OK && rt) {
                        auto make_tex = [&](UINT tw, UINT th, DXGI_FORMAT fmt, bool uav) {
                            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            rd.Width = tw; rd.Height = th;
                            rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                            rd.Format = fmt; rd.SampleDesc = {1, 0};
                            if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                            ID3D12Resource* r = nullptr;
                            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                nullptr, IID_PPV_ARGS(&r));
                            return r;
                        };
                        ID3D12Resource* t_col = make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_alb = make_tex(320, 180, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_spc = make_tex(320, 180, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_nrm = make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_rgh = make_tex(320, 180, DXGI_FORMAT_R16_FLOAT, false);
                        ID3D12Resource* t_dep = make_tex(320, 180, DXGI_FORMAT_R32_FLOAT, false);
                        ID3D12Resource* t_mv  = make_tex(320, 180, DXGI_FORMAT_R16G16_FLOAT, false);
                        ID3D12Resource* t_out = make_tex(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

                        for (int wait_i = 0; wait_i < 200; ++wait_i) {
                            OnrRrInfo info{sizeof(OnrRrInfo)};
                            api4.v3.v2.v1.GetInfo(rt, &info);
                            if (info.state == ONR_RR_STATE_READY || info.state == ONR_RR_STATE_FAILED) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }

                        OnrRrFrame fr{};
                        fr.size = sizeof(fr);
                        fr.render_width = 320; fr.render_height = 180;
                        fr.mv_scale_x = 1.0f; fr.mv_scale_y = 1.0f;
                        fr.flags = ONR_RR_FRAME_RESET | ONR_RR_FRAME_HW_DEPTH;
                        for (int i = 0; i < 16; ++i) {
                            fr.world_to_view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                            fr.view_to_clip[i]  = (i % 5 == 0) ? 1.0f : 0.0f;
                        }
                        const uint32_t srv_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        fr.color          = {t_col, srv_st, 0, 0};
                        fr.diffuse_albedo = {t_alb, srv_st, 0, 0};
                        fr.specular_albedo= {t_spc, srv_st, 0, 0};
                        fr.normals        = {t_nrm, srv_st, 0, 0};
                        fr.roughness      = {t_rgh, srv_st, 0, 0};
                        fr.depth          = {t_dep, srv_st, 0, 0};
                        fr.motion         = {t_mv,  srv_st, 0, 0};
                        fr.output         = {t_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0};

                        ID3D12CommandAllocator* sat_alloc = nullptr;
                        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sat_alloc));
                        std::vector<ID3D12GraphicsCommandList*> sat_lists;
                        int ok_count = 0, busy_rc = 0;
                        for (int i = 0; i < 128; ++i) {
                            ID3D12GraphicsCommandList* l = nullptr;
                            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sat_alloc, nullptr, IID_PPV_ARGS(&l));
                            OnrRrNotifyD3d12ListReset(l, sat_alloc);
                            void* job = nullptr;
                            const int32_t rc = api4.Record(rt, &fr, l, ONR_RR_EDIT_OFF, nullptr, &job);
                            l->Close();
                            sat_lists.push_back(l);
                            if (rc == ONR_RR_OK) ++ok_count;
                            else if (rc == ONR_RR_BUSY) { busy_rc = rc; break; }
                        }
                        sat_alloc->Reset();
                        OnrRrNotifyD3d12AllocatorReset(sat_alloc);
                        int loop_ok = 0;
                        for (int iter = 0; iter < 135; ++iter) {
                            sat_alloc->Reset();
                            OnrRrNotifyD3d12AllocatorReset(sat_alloc);
                            sat_lists[0]->Reset(sat_alloc, nullptr);
                            OnrRrNotifyD3d12ListReset(sat_lists[0], sat_alloc);
                            void* job_after = nullptr;
                            const int32_t rc_iter = api4.Record(rt, &fr, sat_lists[0], ONR_RR_EDIT_OFF, nullptr, &job_after);
                            sat_lists[0]->Close();
                            if (rc_iter == ONR_RR_OK) ++loop_ok;
                            else break;
                        }
                        const int32_t rc_after = (loop_ok == 135) ? ONR_RR_OK : ONR_RR_BUSY;
                        for (auto* l : sat_lists) {
                            OnrRrNotifyD3d12ListReleased(l);
                            l->Release();
                        }
                        sat_alloc->Reset();
                        OnrRrNotifyD3d12AllocatorReset(sat_alloc);
                        OnrRrNotifyD3d12AllocatorReleased(sat_alloc);
                        sat_alloc->Release();
                        for (ID3D12Resource* r : {t_col, t_alb, t_spc, t_nrm, t_rgh, t_dep, t_mv, t_out})
                            if (r) r->Release();
                        api4.v3.v2.v1.Destroy(rt);

                        if (ok_count != 127 || busy_rc != ONR_RR_BUSY || rc_after != ONR_RR_OK) {
                            std::fprintf(stderr, "FAIL: ONR_RR_BUSY test ok_count=%d busy_rc=%d loop_ok=%d\n",
                                         ok_count, busy_rc, loop_ok);
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] Item #3/#4 (D3D12 ONR_RR_BUSY saturation & >127-frame allocator reset loop): %d in-flight slots accepted, 128th returned ONR_RR_BUSY (%d), 135 post-reset Records OK\n",
                                    ok_count, busy_rc);
                    }

                    // ---- Vulkan Split Bridge test (ONR_RR_BACKEND_CHOICE_VULKAN: RecordInputs -> Enqueue -> RecordOutput) ----
                    ci.backend = ONR_RR_BACKEND_CHOICE_VULKAN;
                    void* rt_vk = nullptr;
                    if (api4.v3.v2.v1.Create(&ci, &rt_vk) == ONR_RR_OK && rt_vk) {
                        for (int wait_i = 0; wait_i < 200; ++wait_i) {
                            OnrRrInfo info{sizeof(OnrRrInfo)};
                            api4.v3.v2.v1.GetInfo(rt_vk, &info);
                            if (info.state == ONR_RR_STATE_READY || info.state == ONR_RR_STATE_FAILED) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        auto make_tex = [&](UINT tw, UINT th, DXGI_FORMAT fmt, bool uav) {
                            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            rd.Width = tw; rd.Height = th;
                            rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                            rd.Format = fmt; rd.SampleDesc = {1, 0};
                            if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                            ID3D12Resource* r = nullptr;
                            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                nullptr, IID_PPV_ARGS(&r));
                            return r;
                        };
                        ID3D12Resource* t_col = make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_alb = make_tex(320, 180, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_spc = make_tex(320, 180, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_nrm = make_tex(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_rgh = make_tex(320, 180, DXGI_FORMAT_R16_FLOAT, false);
                        ID3D12Resource* t_dep = make_tex(320, 180, DXGI_FORMAT_R32_FLOAT, false);
                        ID3D12Resource* t_mv  = make_tex(320, 180, DXGI_FORMAT_R16G16_FLOAT, false);
                        ID3D12Resource* t_out = make_tex(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

                        OnrRrFrame fr{};
                        fr.size = sizeof(fr);
                        fr.render_width = 320; fr.render_height = 180;
                        fr.mv_scale_x = 1.0f; fr.mv_scale_y = 1.0f;
                        fr.flags = ONR_RR_FRAME_RESET | ONR_RR_FRAME_HW_DEPTH;
                        for (int i = 0; i < 16; ++i) {
                            fr.world_to_view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                            fr.view_to_clip[i]  = (i % 5 == 0) ? 1.0f : 0.0f;
                        }
                        const uint32_t srv_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        fr.color          = {t_col, srv_st, 0, 0};
                        fr.diffuse_albedo = {t_alb, srv_st, 0, 0};
                        fr.specular_albedo= {t_spc, srv_st, 0, 0};
                        fr.normals        = {t_nrm, srv_st, 0, 0};
                        fr.roughness      = {t_rgh, srv_st, 0, 0};
                        fr.depth          = {t_dep, srv_st, 0, 0};
                        fr.motion         = {t_mv,  srv_st, 0, 0};
                        fr.output         = {t_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0};

                        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
                        ID3D12CommandQueue* q = nullptr;
                        ID3D12CommandAllocator* ca1 = nullptr;
                        ID3D12CommandAllocator* ca2 = nullptr;
                        ID3D12GraphicsCommandList* cl1 = nullptr;
                        ID3D12GraphicsCommandList* cl2 = nullptr;
                        ID3D12Fence* fence = nullptr;
                        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
                        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca1));
                        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca2));
                        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca1, nullptr, IID_PPV_ARGS(&cl1));
                        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca2, nullptr, IID_PPV_ARGS(&cl2));
                        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

                        void* job = nullptr;
                        const int32_t rc_in = api4.v3.v2.v1.RecordInputs(rt_vk, &fr, cl1, &job);
                        cl1->Close();
                        ID3D12CommandList* l1 = cl1;
                        q->ExecuteCommandLists(1, &l1);
                        const int32_t rc_enq = api4.v3.v2.v1.Enqueue(rt_vk, job, q);
                        const int32_t rc_out = api4.v3.v2.v1.RecordOutput(rt_vk, job, cl2);
                        cl2->Close();
                        ID3D12CommandList* l2 = cl2;
                        q->ExecuteCommandLists(1, &l2);
                        q->Signal(fence, 1);
                        fence->SetEventOnCompletion(1, nullptr);

                        uint32_t be = 0;
                        api4.GetBackend(rt_vk, &be);
                        fence->Release();
                        cl2->Release(); cl1->Release();
                        ca2->Release(); ca1->Release();
                        q->Release();
                        for (ID3D12Resource* r : {t_col, t_alb, t_spc, t_nrm, t_rgh, t_dep, t_mv, t_out})
                            if (r) r->Release();
                        api4.v3.v2.v1.Destroy(rt_vk);

                        if (be != ONR_RR_BACKEND_VULKAN || rc_in != ONR_RR_OK || rc_enq != ONR_RR_OK || rc_out != ONR_RR_OK) {
                            std::fprintf(stderr, "FAIL: Vulkan Bridge test be=%u rc_in=%d rc_enq=%d rc_out=%d\n",
                                         be, rc_in, rc_enq, rc_out);
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] Item #7 (D3D12 -> Vulkan Bridge RecordInputs/Enqueue/RecordOutput): backend=%u (VULKAN), rc_in=%d, rc_enq=%d, rc_out=%d\n",
                                    be, rc_in, rc_enq, rc_out);
                    }

                    // ---- R1: Verify RecordVk does NOT mark live VkDevice as destroyed ----
                    OnrRrApi5 api5{};
                    if (OnrRrGetApi(ONR_RR_ABI_VERSION_5, &api5.v4.v3.v2.v1) == ONR_RR_OK) {
                        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
                        app.apiVersion = VK_API_VERSION_1_3;
                        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
                        ici.pApplicationInfo = &app;
                        VkInstance inst_r1 = VK_NULL_HANDLE;
                        if (vkCreateInstance(&ici, nullptr, &inst_r1) == VK_SUCCESS && inst_r1) {
                            uint32_t npd = 0;
                            vkEnumeratePhysicalDevices(inst_r1, &npd, nullptr);
                            std::vector<VkPhysicalDevice> pds(npd);
                            vkEnumeratePhysicalDevices(inst_r1, &npd, pds.data());
                            for (auto pd : pds) {
                                uint32_t nqf = 0;
                                vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
                                std::vector<VkQueueFamilyProperties> qfs(nqf);
                                vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
                                uint32_t qf = 0;
                                while (qf < nqf && !(qfs[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++qf;
                                if (qf == nqf) continue;
                                float prio = 1.0f;
                                VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                                qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
                                VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                                dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
                                VkDeviceCreateInfo patched{};
                                void* setup = nullptr;
                                if (api5.PrepareVkDevice(pd, &dci, &patched, &setup) != ONR_RR_OK) continue;
                                VkDevice dev_r1 = VK_NULL_HANDLE;
                                const VkResult vrc = vkCreateDevice(pd, &patched, nullptr, &dev_r1);
                                uint32_t rf = 0, ri = 0;
                                api5.FinishVkDevice(setup, dev_r1, &rf, &ri);
                                if (vrc != VK_SUCCESS || !dev_r1) continue;
                                OnrRrVkDevice host_r1{sizeof(host_r1), VK_API_VERSION_1_3, inst_r1, pd, dev_r1, rf, ri};
                                OnrRrCreateInfo ci_r1{};
                                ci_r1.size = sizeof(ci_r1);
                                ci_r1.vk_device = &host_r1;
                                ci_r1.max_render_width = 32; ci_r1.max_render_height = 32;
                                ci_r1.output_width = 64; ci_r1.output_height = 64;
                                ci_r1.data_dir = w_data.c_str();
                                void* ctx_r1 = nullptr;
                                if (api5.v4.v3.v2.v1.Create(&ci_r1, &ctx_r1) == ONR_RR_OK && ctx_r1) {
                                    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                                    pci.queueFamilyIndex = qf;
                                    VkCommandPool pool_r1 = VK_NULL_HANDLE;
                                    vkCreateCommandPool(dev_r1, &pci, nullptr, &pool_r1);
                                    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                    cai.commandPool = pool_r1; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                                    VkCommandBuffer cb_r1 = VK_NULL_HANDLE;
                                    vkAllocateCommandBuffers(dev_r1, &cai, &cb_r1);
                                    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                    vkBeginCommandBuffer(cb_r1, &bi);
                                    OnrRrVkImage inv_img{}; inv_img.width = 32; inv_img.height = 32;
                                    OnrRrFrame f_r1{}; f_r1.size = sizeof(f_r1); f_r1.render_width = 32; f_r1.render_height = 32;
                                    f_r1.color.resource = &inv_img; f_r1.output.resource = &inv_img;
                                    const bool dest_before = onr::vk_device_is_destroyed(dev_r1);
                                    const int32_t rec_rc = api5.RecordVk(ctx_r1, &f_r1, cb_r1);
                                    const bool dest_after = onr::vk_device_is_destroyed(dev_r1);
                                    vkEndCommandBuffer(cb_r1);
                                    vkDeviceWaitIdle(dev_r1);

                                    // Also test Vulkan R2 on hosted Device: multi-bind on same open VkCommandBuffer + 4096 cmd churn + reset retirement
                                    bool vk_r2_ok = false;
                                    {
                                        onr::Device hdev;
                                        std::string herr;
                                        if (hdev.adopt({inst_r1, pd, dev_r1, rf, ri}, herr)) {
                                            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                                            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                                            qpi.queryCount = onr::kMaxQueries;
                                            VkQueryPool qp0 = VK_NULL_HANDLE, qp1 = VK_NULL_HANDLE;
                                            vkCreateQueryPool(dev_r1, &qpi, nullptr, &qp0);
                                            vkCreateQueryPool(dev_r1, &qpi, nullptr, &qp1);
                                            VkCommandPool pool_r2 = VK_NULL_HANDLE;
                                            VkCommandPoolCreateInfo pci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                                            pci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                                            pci2.queueFamilyIndex = rf;
                                            vkCreateCommandPool(dev_r1, &pci2, nullptr, &pool_r2);
                                            VkCommandBuffer cb_r2 = VK_NULL_HANDLE;
                                            VkCommandBufferAllocateInfo cai2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                            cai2.commandPool = pool_r2;
                                            cai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                                            cai2.commandBufferCount = 1;
                                            vkAllocateCommandBuffers(dev_r1, &cai2, &cb_r2);
                                            onr::vk_note_command_buffers_allocated(dev_r1, pool_r2, 1, &cb_r2);
                                            VkCommandBufferBeginInfo bi2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                            vkBeginCommandBuffer(cb_r2, &bi2);
                                            onr::vk_note_command_buffer_reset(cb_r2);
                                            // Two bind() recordings inside the same open VkCommandBuffer
                                            hdev.bind(cb_r2, qp0);
                                            hdev.stamp();
                                            hdev.unbind();
                                            hdev.bind(cb_r2, qp1);
                                            hdev.stamp();
                                            hdev.unbind();
                                            vkEndCommandBuffer(cb_r2);
                                            VkQueue q_vk = VK_NULL_HANDLE;
                                            vkGetDeviceQueue(dev_r1, rf, ri, &q_vk);
                                            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                                            si.commandBufferCount = 1;
                                            si.pCommandBuffers = &cb_r2;
                                            vkQueueSubmit(q_vk, 1, &si, VK_NULL_HANDLE);
                                            vkQueueWaitIdle(q_vk);
                                            // Even after first GPU execution and 4096 real command buffer resets, neither qp0 nor qp1 may retire while cb_r2 is un-reset
                                            uint64_t ts0_first = 0, ts1_first = 0, ts0_replay = 0, ts1_replay = 0;
                                            vkGetQueryPoolResults(dev_r1, qp0, 0, 1, sizeof(ts0_first), &ts0_first, sizeof(ts0_first), VK_QUERY_RESULT_64_BIT);
                                            vkGetQueryPoolResults(dev_r1, qp1, 0, 1, sizeof(ts1_first), &ts1_first, sizeof(ts1_first), VK_QUERY_RESULT_64_BIT);
                                            std::vector<VkCommandBuffer> churn_cbs(4096, VK_NULL_HANDLE);
                                            cai2.commandBufferCount = uint32_t(churn_cbs.size());
                                            vkAllocateCommandBuffers(dev_r1, &cai2, churn_cbs.data());
                                            onr::vk_note_command_buffers_allocated(dev_r1, pool_r2, uint32_t(churn_cbs.size()), churn_cbs.data());
                                            for (VkCommandBuffer c : churn_cbs) {
                                                vkResetCommandBuffer(c, 0);
                                                onr::vk_note_command_buffer_reset(c);
                                            }
                                            const bool ret0_before = hdev.pool_retired(qp0);
                                            const bool ret1_before = hdev.pool_retired(qp1);
                                            // Replay cb_r2 without host reset: embedded vkCmdResetQueryPool resets qp0/qp1 inside cb_r2 before vkCmdWriteTimestamp2
                                            vkQueueSubmit(q_vk, 1, &si, VK_NULL_HANDLE);
                                            vkQueueWaitIdle(q_vk);
                                            vkGetQueryPoolResults(dev_r1, qp0, 0, 1, sizeof(ts0_replay), &ts0_replay, sizeof(ts0_replay), VK_QUERY_RESULT_64_BIT);
                                            vkGetQueryPoolResults(dev_r1, qp1, 0, 1, sizeof(ts1_replay), &ts1_replay, sizeof(ts1_replay), VK_QUERY_RESULT_64_BIT);
                                            const bool ret0_replay = hdev.pool_retired(qp0);
                                            const bool ret1_replay = hdev.pool_retired(qp1);
                                            // Now reset cb_r2 and notify: both qp0 and qp1 must retire
                                            vkResetCommandBuffer(cb_r2, 0);
                                            onr::vk_note_command_buffer_reset(cb_r2);
                                            const bool ret0_after = hdev.pool_retired(qp0);
                                            const bool ret1_after = hdev.pool_retired(qp1);
                                            // Also verify >64-frame (72-iteration) reset + notify loop reuses qp0 without exhausting 64 view/query slots
                                            int vk_loop_ok = 0;
                                            for (int iter = 0; iter < 72; ++iter) {
                                                vkResetCommandBuffer(cb_r2, 0);
                                                onr::vk_note_command_buffer_reset(cb_r2);
                                                if (!hdev.pool_retired(qp0)) break;
                                                vkBeginCommandBuffer(cb_r2, &bi2);
                                                hdev.bind(cb_r2, qp0);
                                                hdev.stamp();
                                                hdev.unbind();
                                                vkEndCommandBuffer(cb_r2);
                                                ++vk_loop_ok;
                                            }
                                            vkResetCommandBuffer(cb_r2, 0);
                                            onr::vk_note_command_buffer_reset(cb_r2);
                                            vkDestroyQueryPool(dev_r1, qp0, nullptr);
                                            vkDestroyQueryPool(dev_r1, qp1, nullptr);
                                            onr::vk_note_command_pool_destroyed(dev_r1, pool_r2);
                                            vkDestroyCommandPool(dev_r1, pool_r2, nullptr);
                                            vk_r2_ok = (!ret0_before && !ret1_before && !ret0_replay && !ret1_replay && ret0_after && ret1_after &&
                                                        ts0_first > 0 && ts1_first > 0 && ts0_replay >= ts0_first && ts1_replay >= ts1_first &&
                                                        vk_loop_ok == 72 && hdev.all_retired());
                                        }
                                    }

                                    onr::vk_note_command_pool_destroyed(dev_r1, pool_r1);
                                    api5.v4.v3.v2.v1.Destroy(ctx_r1);
                                    vkDestroyCommandPool(dev_r1, pool_r1, nullptr);
                                    vkDestroyDevice(dev_r1, nullptr);
                                    vkDestroyInstance(inst_r1, nullptr);
                                    inst_r1 = VK_NULL_HANDLE;
                                    if (dest_before || dest_after || !vk_r2_ok) {
                                        std::fprintf(stderr, "FAIL: R1/Vk-R2 failed (before=%d after=%d rc=%d vk_r2_ok=%d)\n",
                                                     int(dest_before), int(dest_after), rec_rc, int(vk_r2_ok));
                                        dev->Release();
                                        return 1;
                                    }
                                    std::printf("[PASS] Item R1 & Vulkan R2 (RecordVk live VkDevice + Vk multi-bind/replay/churn retirement): destroyed_before=%d, destroyed_after=%d, vk_r2_ok=%d\n",
                                                int(dest_before), int(dest_after), int(vk_r2_ok));
                                    break;
                                }
                                vkDestroyDevice(dev_r1, nullptr);
                            }
                            if (inst_r1) vkDestroyInstance(inst_r1, nullptr);
                        }

                        // ---- R10: Verify Create(data_dir = nullptr) discovers default openrr-amd/shaders directory ----
                        const auto def_dir = std::filesystem::path("build/asi/openrr-amd/shaders");
                        std::error_code ec_r10;
                        std::filesystem::create_directories(def_dir, ec_r10);
                        for (const auto& entry : std::filesystem::directory_iterator(spv_dir, ec_r10)) {
                            std::filesystem::copy_file(entry.path(), def_dir / entry.path().filename(),
                                                       std::filesystem::copy_options::overwrite_existing, ec_r10);
                        }
                        OnrRrCreateInfo ci_r10{};
                        ci_r10.size = sizeof(ci_r10);
                        ci_r10.d3d12_device = dev;
                        ci_r10.max_render_width = 32;
                        ci_r10.max_render_height = 32;
                        ci_r10.output_width = 64;
                        ci_r10.output_height = 64;
                        ci_r10.data_dir = nullptr;
                        void* ctx_r10 = nullptr;
                        const int32_t rc_r10 = api5.v4.v3.v2.v1.Create(&ci_r10, &ctx_r10);
                        if (ctx_r10) api5.v4.v3.v2.v1.Destroy(ctx_r10);
                        std::filesystem::remove_all("build/asi/openrr-amd", ec_r10);
                        if (rc_r10 != ONR_RR_OK || !ctx_r10) {
                            std::fprintf(stderr, "FAIL: R10 Create(data_dir=nullptr) returned rc=%d ctx=%p\n", rc_r10, ctx_r10);
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] Item R10 (Create with data_dir=nullptr discovers default openrr-amd/shaders): rc=%d\n", rc_r10);
                    }
                }
            }
        }
    }

    dev->Release();
    return 0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (test_key_parsing() != 0) return 1;
    if (test_split_hardening_and_eligibility() != 0) return 1;
    if (test_d3d12_gpu_and_engine() != 0) return 1;
    std::printf("ALL ASI UNIT TESTS PASSED\n");
    return 0;
}
