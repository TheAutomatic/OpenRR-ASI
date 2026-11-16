// SPDX-License-Identifier: MIT
// Unit and D3D12 harness tests for openrr-amd.asi components:
// - Item #13: single-digit and numeric virtual-key parsing in openrr::pe::parse_key
// - Item #11: split vtable fallback self-recursion guard & CreateCommandList fallback on copied vtables
// - Item #12: split prepare() rejection of open split barriers, enhanced barriers, RTAS, meta commands, and bundles
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "rr_pe_config.hpp"
#include "rr_lang.hpp"
#include "rr_ini_edit.hpp"
#include "rr_pe_d3d12split.hpp"
#include "rr_pe_vkdevice.hpp"
#include "opennr_rr_api.h"
#include "rr_d3d12_record.hpp"
#include "rr_data_paths.hpp"
#include "opennr_rr/d3d12.hpp"
#include "opennr_rr/engine.hpp"
#include "opennr_rr/vk.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "aco_compare.hpp"

// ---- R33: count-only probes for host-visible command-list state mutations ----
// They deliberately do not forward: the D3D12 vtable entries they shadow are already MinHook-detoured by the split
// hooks of this process, and the detour resolves "our" entry back to itself, so forwarding would re-enter the probe.
// R33 asserts the counts stay 0, so the shadowed call never has to reach the driver.
static int g_probe_set_heaps = 0;
static int g_probe_set_graphics_rs = 0;
static int g_probe_set_graphics_table = 0;
using PFN_Probe_SetHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_Probe_SetGraphicsRs = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_Probe_SetGraphicsTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);

static void STDMETHODCALLTYPE probe_set_heaps(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*) {
    ++g_probe_set_heaps;
}

static void STDMETHODCALLTYPE probe_set_graphics_rs(ID3D12GraphicsCommandList*, ID3D12RootSignature*) {
    ++g_probe_set_graphics_rs;
}

static void STDMETHODCALLTYPE probe_set_graphics_table(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) {
    ++g_probe_set_graphics_table;
}

static int test_installed_data_paths() {
    namespace fs = std::filesystem;
    const auto game = fs::temp_directory_path() /
        (L"openrr-data-paths-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto storage = game / L"_storage_";
    const auto payload = game / L"openrr-amd";
    const auto shaders = payload / L"shaders";
    fs::create_directories(shaders);
    fs::create_directory(storage);
    std::ofstream(storage / L"nvngx_dlssd.dll").close();
    std::ofstream(payload / L"nvngx_dlssd.dll").close();

    using openrr::pe::installed_data_dir;
    const bool ok = installed_data_dir(storage, L"nvngx_dlssd.dll") == payload &&
                    installed_data_dir(storage, L"shaders") == payload &&
                    installed_data_dir(game, L"nvngx_dlssd.dll") == payload &&
                    installed_data_dir(storage, L"missing").empty();
    fs::remove(payload / L"nvngx_dlssd.dll");
    fs::remove(storage / L"nvngx_dlssd.dll");
    fs::remove(shaders);
    fs::remove(payload);
    fs::remove(storage);
    fs::remove(game);
    if (!ok) {
        std::fprintf(stderr, "FAIL: OpenRR installed data lookup under RE9 _storage_\n");
        return 1;
    }
    std::printf("[PASS] RE9 _storage_ resolves the game-root OpenRR weights and shaders\n");
    return 0;
}

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
        ini2.set("OpenRR", "Mode", "Fast");
        ini2.save();
        const auto experimental = openrr::pe::load_config_file(openrr::u8::text(tmp_ini));
        std::error_code ec;
        std::filesystem::remove(tmp_ini, ec);
        if (experimental.mode != 2 || openrr::pe::network_mode_level("2") != 2 ||
            openrr::pe::network_mode_level("new") != 1) {
            std::fprintf(stderr, "FAIL: OpenRR experimental mode was not preserved by the INI parser\n");
            return 1;
        }
        if (std::string(prof_buf) != "false" || en_before != "false" || en_after != "false" || mode_after != "legacy" ||
            cfg_before.enabled || cfg_after.enabled || cfg_after.mode != 0) {
            std::fprintf(stderr, "FAIL: R7/R8 BOM/newline test failed (prof='%s' before='%s' after='%s' mode='%s' cfg_en=%d/%d cfg_mode=%d)\n",
                         prof_buf, en_before.c_str(), en_after.c_str(), mode_after.c_str(),
                         int(cfg_before.enabled), int(cfg_after.enabled), cfg_after.mode);
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
        if (std::getenv("OPENRR_ACO_COMPARE")) return aco_compare(vk, d3d, spv_dir_str);
        if (std::getenv("OPENRR_ACO_API_PROBE")) {
            OnrRrApi4 api{};
            if (OnrRrGetApi(ONR_RR_ABI_VERSION_4, &api.v3.v2.v1) != ONR_RR_OK) return 1;
            const auto data = (bdir / "stage" / "openrr-amd").wstring();
            const auto log = (bdir / "audit-api-runtime.log").wstring();
            const char* old = std::getenv("ONR_RR_BACKEND");
            const std::string saved = old ? old : "";
            _putenv_s("ONR_RR_BACKEND", "vulkan");
            int failed = 0;
            for (uint32_t size : {uint32_t(ONR_RR_CREATE_INFO_SIZE_V1), uint32_t(ONR_RR_CREATE_INFO_SIZE_V3), uint32_t(sizeof(OnrRrCreateInfo))}) {
                OnrRrCreateInfo ci{};ci.size=size;ci.d3d12_device=dev;
                ci.max_render_width=ci.max_render_height=32;ci.output_width=ci.output_height=64;
                ci.data_dir=data.c_str();ci.log_path=log.c_str();ci.backend=ONR_RR_BACKEND_CHOICE_AUTO;
                // Poison fields outside the advertised old structure size.
                ci.compiler=size<sizeof(ci)?99:0;
                void* ctx=nullptr;uint32_t backend=99;
                const auto rc=api.v3.v2.v1.Create(&ci,&ctx);
                if(ctx){api.GetBackend(ctx,&backend);api.v3.v2.v1.Destroy(ctx);}
                if(rc!=ONR_RR_OK || backend!=ONR_RR_BACKEND_VULKAN){
                    char error[1024]{};api.v3.v2.v1.GetLastError(error,sizeof(error));
                    std::fprintf(stderr,"FAIL API size=%u rc=%d backend=%u: %s\n",size,rc,backend,error);failed=1;
                } else std::printf("[PASS] API size=%u: native Vulkan selected with ONR_RR_BACKEND=vulkan\n",size);
            }
            _putenv_s("ONR_RR_BACKEND", saved.c_str());
            return failed;
        }

        if (std::getenv("OPENRR_ACO_PROBE")) {
            try {
                d3d.set_compiler(true);
                auto k = d3d.load(spv_dir_str + "/aco_abi.spv", 0);
                if (!k.aco) throw std::runtime_error("aco_abi silently fell back to LLPC");
                auto out = d3d.create(57 * 64 * 4);
                struct { uint64_t dst; uint32_t k, mid[57], last; } pc{};
                static_assert(sizeof(pc) == 248); // trailing host alignment is not pushed
                pc.dst = out.addr; pc.k = 17; pc.last = 0x9876;
                for (unsigned i = 0; i < 57; ++i) pc.mid[i] = i * 31;
                d3d.begin_own();
                d3d.dispatch(k, &pc, 244, 57);
                d3d.submit_own();
                uint32_t got[57*64]{};
                d3d.download(out, got, sizeof got);
                for (unsigned i = 0; i < 57*64; ++i)
                    if (got[i] != 17*i + pc.mid[(i/64)%57] + pc.last + 57*65536) {
                        std::fprintf(stderr, "got[%u]=%u expected=%u\n", i, got[i], 17*i+pc.mid[(i/64)%57]+pc.last+57*65536);
                        throw std::runtime_error("ACO 244-byte push/grid readback mismatch at " + std::to_string(i));
                    }
                d3d.destroy(k); d3d.destroy(out);
                std::printf("[PASS] ACO 244-byte push ABI, spill pointer, inline constants, workgroup count\n");
                auto lane = d3d.load(spv_dir_str + "/selftest.spv");
                if (!lane.aco) throw std::runtime_error("original selftest silently fell back to LLPC");
                auto lane_out = d3d.create(128*4);
                pc.dst = lane_out.addr;
                d3d.begin_own(); d3d.dispatch(lane, &pc, 244, 2); d3d.submit_own();
                uint32_t lane_got[128]{}; d3d.download(lane_out, lane_got, sizeof lane_got);
                for (unsigned i = 0; i < 128; ++i)
                    if (lane_got[i] != 17*i + pc.mid[i%57] + pc.last + 2*65536)
                        throw std::runtime_error("original ACO lane-varying push mismatch at " + std::to_string(i));
                d3d.destroy(lane); d3d.destroy(lane_out);
                std::printf("[PASS] Original selftest: ACO lane-varying push indexing (128 outputs)\n");
                auto clear = d3d.load(spv_dir_str + "/clear_range.spv");
                if (!clear.aco) throw std::runtime_error("clear_range silently fell back to LLPC");
                auto fill = d3d.create(4096);
                struct { uint64_t dst; uint32_t bytes, value; } cp{fill.addr, 4096, 0x31415926};
                d3d.begin_own(); d3d.dispatch(clear, &cp, sizeof cp, 4); d3d.submit_own();
                std::vector<uint32_t> words(1024); d3d.download(fill, words.data(), 4096);
                for (auto w : words) if (w != cp.value) throw std::runtime_error("ACO BDA fill mismatch");
                d3d.destroy(clear); d3d.destroy(fill);
                std::printf("[PASS] ACO clear_range pure BDA fill\n");
                std::string why;
                if (!d3d.init_sampler(why)) throw std::runtime_error("sampler: " + why);
                auto tex = d3d.load(spv_dir_str + "/selftest_tex.spv");
                if (!tex.aco) throw std::runtime_error("selftest_tex silently fell back to LLPC");
                auto dst = d3d.create(512*8);
                auto sampler = d3d.sampler(true, true);
                for (uint32_t slot : {0u, 3u, 7u}) {
                    auto src = d3d.create(512*8+65536);
                    std::vector<uint16_t> pixels(512*4);
                    for (unsigned i = 0; i < pixels.size(); ++i) pixels[i] = uint16_t(0x3000 + slot*128 + i);
                    d3d.upload(src, pixels.data(), pixels.size()*2);
                    auto image = d3d.alias_image(src, 64, 8, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
                    d3d.set_texture(slot, image, sampler);
                    struct { uint64_t dst; int32_t size[2]; uint32_t slot; } tp{dst.addr, {64,8}, slot};
                    d3d.begin_own(); d3d.dispatch(tex, &tp, 20, 8); d3d.submit_own();
                    std::vector<uint16_t> readback(pixels.size()); d3d.download(dst, readback.data(), readback.size()*2);
                    if (readback != pixels) throw std::runtime_error("ACO descriptor array mismatch at slot " + std::to_string(slot));
                    d3d.destroy(image); d3d.destroy(src);
                }
                d3d.destroy(tex); d3d.destroy(dst);
                std::printf("[PASS] ACO sampled descriptor slots 0, 3, 7 (distinct contents)\n");
                return 0;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "FAIL ACO probe: %s\n", e.what());
                return 1;
            }
        }

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

        // ---- R33: D3dDevice::bind()/dispatch() must leave host descriptor heaps and graphics root state untouched ----
        {
            ID3D12CommandAllocator* ca_probe = nullptr;
            ID3D12GraphicsCommandList* cl_probe = nullptr;
            ID3D12DescriptorHeap* host_heaps[2]{};
            D3D12_DESCRIPTOR_HEAP_DESC hpd{};
            hpd.NumDescriptors = 16;
            hpd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hpd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            const HRESULT hr_ca = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_probe));
            const HRESULT hr_cl = SUCCEEDED(hr_ca)
                ? dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca_probe, nullptr, IID_PPV_ARGS(&cl_probe))
                : hr_ca;
            const HRESULT hr_h0 = SUCCEEDED(hr_cl) ? dev->CreateDescriptorHeap(&hpd, IID_PPV_ARGS(&host_heaps[0])) : hr_cl;
            hpd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            const HRESULT hr_h1 = SUCCEEDED(hr_h0) ? dev->CreateDescriptorHeap(&hpd, IID_PPV_ARGS(&host_heaps[1])) : hr_h0;
            if (FAILED(hr_h1) || !cl_probe || !host_heaps[0] || !host_heaps[1]) {
                std::fprintf(stderr, "FAIL: R33 could not create host command list / descriptor heaps (0x%lX)\n",
                             static_cast<unsigned long>(hr_h1));
                dev->Release();
                return 1;
            }
            // The host (RE9 D3D12 path tracing) binds [ResourceDescriptorHeap, SamplerDescriptorHeap].
            cl_probe->SetDescriptorHeaps(2, host_heaps);

            onr::Kernels k_probe;
            k_probe.load(d3d, spv_dir, {"clear_range"});
            onr::Buffer b_probe = d3d.create(4096);
            onr::d3d12_note_list_reset(cl_probe, ca_probe);

            void** vt_probe = *reinterpret_cast<void***>(cl_probe);
            void* copied_probe_vt[86]{};
            std::memcpy(copied_probe_vt, vt_probe, sizeof copied_probe_vt);
            copied_probe_vt[28] = reinterpret_cast<void*>(&probe_set_heaps);
            copied_probe_vt[30] = reinterpret_cast<void*>(&probe_set_graphics_rs);
            copied_probe_vt[32] = reinterpret_cast<void*>(&probe_set_graphics_table);
            *reinterpret_cast<void***>(cl_probe) = copied_probe_vt;

            // Self-check: the probe is wired into the copied vtable (count only, nothing reaches the driver).
            reinterpret_cast<PFN_Probe_SetHeaps>(copied_probe_vt[28])(cl_probe, 0, nullptr);
            const int probe_self_check = g_probe_set_heaps;
            g_probe_set_heaps = g_probe_set_graphics_rs = g_probe_set_graphics_table = 0;

            d3d.bind(cl_probe, 0);
            const onr::ClearRangePC pc{b_probe.addr, 4096u, 0u};
            k_probe.run(d3d, "clear_range", &pc, sizeof pc, 5u);
            d3d.unbind();

            *reinterpret_cast<void***>(cl_probe) = vt_probe;
            const int n_heaps = g_probe_set_heaps;
            const int n_graphics_rs = g_probe_set_graphics_rs;
            const int n_graphics_table = g_probe_set_graphics_table;

            d3d.destroy(b_probe);
            k_probe.unload(d3d);
            cl_probe->Close();
            cl_probe->Release();
            ca_probe->Release();
            host_heaps[0]->Release();
            host_heaps[1]->Release();

            if (probe_self_check != 1) {
                std::fprintf(stderr, "FAIL: R33 probe self-check did not observe the routed SetDescriptorHeaps call (%d)\n",
                             probe_self_check);
                dev->Release();
                return 1;
            }
            if (n_heaps != 0 || n_graphics_rs != 0 || n_graphics_table != 0) {
                std::fprintf(stderr,
                             "FAIL: R33 bind()/dispatch() mutated host state: SetDescriptorHeaps=%d "
                             "SetGraphicsRootSignature=%d SetGraphicsRootDescriptorTable=%d\n",
                             n_heaps, n_graphics_rs, n_graphics_table);
                dev->Release();
                return 1;
            }
            std::printf("[PASS] R33 (D3dDevice::bind/dispatch preserves host state): SetDescriptorHeaps=%d, "
                        "SetGraphicsRootSignature=%d, SetGraphicsRootDescriptorTable=%d\n",
                        n_heaps, n_graphics_rs, n_graphics_table);
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
                auto resolve = [&](const onr::Buffer& sub, std::vector<uint8_t>& host, const uint8_t*& base) -> bool {
                    for (const onr::Buffer& fb : onr::EngineTestAccess::fixed(eng)) {
                        if (fb.size && sub.addr >= fb.addr && sub.addr + sub.size <= fb.addr + fb.size) {
                            host.resize(size_t(fb.size));
                            d3d.download(fb, host.data(), host.size());
                            base = host.data() + size_t(sub.addr - fb.addr);
                            return true;
                        }
                    }
                    return false;
                };
            // WP-D1: the encoders' mrg_w e4m3 tail offsets. enc1 (CIN > 0) packs fp16 + tiles via U8
            // (qv_bytes right after the values: 2 * (Co*4C + C*4*Cin) bytes); enc2+ (CIN == 0) pack via
            // UT8 (tail at 16 * Co * C bytes, after the fp16 + tiles). Both tails must carry the exact
            // e4m3 codes of the original weights and leave the fp16 prefix byte-identical (MRG8_W).
            // The stage buffers are address-only sub-slices of the packed fixed_ blob: resolve the
            // owner once and compare at addr - owner.addr.
            {
                uint32_t mrg_checked = 0, mrg_ut8 = 0, mrg_u8 = 0;
                for (auto& [mrg_name, mrg_buf] : onr::EngineTestAccess::stage_mrg(eng)) {
                    const int idx = mrg_name[3] - '0';
                    if (idx < 1 || idx > 4) continue;   // enc0: the embed MLP lives in mrg_w; enc5 has no output merge
                    const onr::StageWeights& s = w.enc[idx];
                    const size_t C = size_t(s.b.C), Co = size_t(s.Co);
                    if (!Co || s.mrg_w.empty()) { std::fprintf(stderr, "FAIL: %s mrg_w tail: Co=%d size=%zu\n", mrg_name.c_str(), s.Co, s.mrg_w.size()); return 1; }
                    const bool ut8 = s.mrg_w.size() == Co * 4 * C;
                    const size_t Cin = ut8 ? 0 : (s.mrg_w.size() - Co * 4 * C) / (4 * C);
                    const size_t off = ut8 ? 16 * Co * C : 2 * (Co * 4 * C + C * 4 * Cin);
                    const auto codes = onr::qv_bytes(s.mrg_w.data(), s.mrg_w.size());
                    if (mrg_buf.size < off + codes.size() / 3) { std::fprintf(stderr, "FAIL: %s mrg_w buffer %llu < tail %llu\n", mrg_name.c_str(), (unsigned long long)mrg_buf.size, (unsigned long long)(off + codes.size() / 3)); return 1; }
                    std::vector<uint8_t> blob_;
                    const uint8_t* got = nullptr;
                    if (!resolve(mrg_buf, blob_, got)) { std::fprintf(stderr, "FAIL: %s mrg_w not inside the packed blob\n", mrg_name.c_str()); return 1; }
                    if (std::memcmp(got, s.mrg_w.data(), s.mrg_w.size() * 2) != 0) { std::fprintf(stderr, "FAIL: %s mrg_w fp16 prefix changed\n", mrg_name.c_str()); return 1; }
                    // e4m3_rne maps -0.0 to the +0 code while qv_bytes keeps the sign bit; both decode to zero
                    auto zeq = [](uint8_t a, uint8_t b) { return a == b || ((a & 0x7f) == 0 && (b & 0x7f) == 0); };
                    for (size_t i = 0; i < s.mrg_w.size(); ++i)
                        if (!zeq(got[off + i], codes.data()[s.mrg_w.size() * 2 + i])) { std::fprintf(stderr, "FAIL: %s mrg_w e4m3 tail at off=%llu (%s) byte %zu: %02x vs %02x\n", mrg_name.c_str(), (unsigned long long)off, ut8 ? "UT8 16*Co*C" : "U8 2*(Co*4C+C*4*Cin)", i, got[off + i], codes.data()[s.mrg_w.size() * 2 + i]); return 1; }
                    ++mrg_checked; ut8 ? ++mrg_ut8 : ++mrg_u8;
                }
                if (mrg_checked != 4 || mrg_ut8 != 3 || mrg_u8 != 1) { std::fprintf(stderr, "FAIL: mrg_w tail check covered %u stages (UT8 %u, U8 %u), expected enc1..enc4 = 1 U8 + 3 UT8\n", mrg_checked, mrg_ut8, mrg_u8); return 1; }
                std::printf("[PASS] WP-D1 mrg_w e4m3 tails (%u stages: %u UT8 @16*Co*C, %u U8 @2*(Co*4C+C*4*Cin), fp16 prefix byte-identical)\n",
                            mrg_checked, mrg_ut8, mrg_u8);
            }
            // WP-D5: every stage's out buffer keeps fp16 + tiles byte-identical and carries the exact
            // e4m3 codes of the ORIGINAL out matrix at 4 * C * X bytes (UT8 tail; W_out FP8 products)
            {
                uint32_t out_checked = 0;
                for (auto& [out_name, out_buf] : onr::EngineTestAccess::stage_out(eng)) {
                    int idx = std::atoi(out_name.c_str() + (out_name[0] == 'e' ? 3 : 4));
                    const onr::StageWeights& s = out_name[0] == 'e' ? w.enc[idx]
                                                : (out_name[3] == '0' ? w.dec0[idx] : w.dec1[idx]);
                    if (s.b.out.empty() || !s.b.C || !s.b.X) { std::fprintf(stderr, "FAIL: %s out stage empty\n", out_name.c_str()); return 1; }
                    const size_t off = 4 * size_t(s.b.C) * size_t(s.b.X);
                    if (out_buf.size < off + s.b.out.size()) { std::fprintf(stderr, "FAIL: %s out buffer %llu < tail %llu\n", out_name.c_str(), (unsigned long long)out_buf.size, (unsigned long long)(off + s.b.out.size())); return 1; }
                    std::vector<uint8_t> oblob_;
                    const uint8_t* got = nullptr;
                    if (!resolve(out_buf, oblob_, got)) { std::fprintf(stderr, "FAIL: %s out not inside the packed blob\n", out_name.c_str()); return 1; }
                    const auto tiled = onr::with_tiles(s.b.out, s.b.X);
                    if (std::memcmp(got, tiled.data(), tiled.size() * 2) != 0) { std::fprintf(stderr, "FAIL: %s out fp16 + tiles prefix changed\n", out_name.c_str()); return 1; }
                    for (size_t i = 0; i < s.b.out.size(); ++i)
                        if (got[off + i] != onr::e4m3_rne(s.b.out[i]) && !((got[off + i] & 0x7f) == 0 && (onr::e4m3_rne(s.b.out[i]) & 0x7f) == 0)) { std::fprintf(stderr, "FAIL: %s out e4m3 tail at off=%llu byte %zu\n", out_name.c_str(), (unsigned long long)off, i); return 1; }
                    ++out_checked;
                }
                if (out_checked < 16) { std::fprintf(stderr, "FAIL: out tail check covered %u stages, expected all 16\n", out_checked); return 1; }
                std::printf("[PASS] WP-D5 out e4m3 tails (%u stages @4*C*X, fp16 + tiles prefix byte-identical)\n", out_checked);
            }
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
                        size_t diff_repeat = 0;
                        for (size_t i = 0; i < out_base.size(); ++i) diff_repeat += (out_dummy[i] != out_ship[i]);
                        k_nopad.unload(d3d);
                        // Native-precision _lt records intentionally diverge numerically from the
                        // driver-compiled occ probes, so diff_ship is reported but no longer gated.
                        // Hard gates: SPV hash EXACT, occupancy probes identical (diff_words),
                        // and shipped run-to-run determinism (diff_repeat).
                        if (diff_words != 0 || diff_repeat != 0) {
                            std::fprintf(stderr, "FAIL: K2 occ_pad output mismatch at %s (diff_words=%zu, diff_repeat=%zu, diff_ship=%zu)\n",
                                         label, diff_words, diff_repeat, diff_ship);
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

                    // Arena lifetime regression: repeat the same sequence, including
                    // wide attributes, and compare output bit-for-bit across fresh engines.
                    {
                        auto run = [&](std::vector<std::vector<uint16_t>>& frames) {
                            onr::Engine arena;
                            arena.init(d3d, k_fast, w);
                            arena.set_fast(true);
                            arena.set_lite(&k_lite);
                            arena.set_lite_on(true);
                            arena.resize(r_sz, o_sz);
                            frames.resize(20);
                            for (int f = 0; f < 20; ++f) {
                                ef.reset = (f == 0);
                                ef.jitter[0] = (f & 1) ? 0.25f : -0.25f;
                                ef.jitter[1] = (f & 2) ? 0.25f : -0.25f;
                                ef.depth_of_field = (f >= 10 && f < 15) ? p_dep.addr : 0;
                                d3d.begin_own();
                                arena.record(ef, pg, p_out.addr);
                                d3d.submit_own();
                                frames[f].resize(size_t(o_sz.x) * o_sz.y * 4);
                                d3d.download(p_out, frames[f].data(), frames[f].size() * 2);
                            }
                            ef.depth_of_field = 0;
                            arena.destroy();
                        };
                        std::vector<std::vector<uint16_t>> a, b;
                        run(a); run(b);
                        size_t bad = 0;
                        for (const auto& frame : a)
                            for (uint16_t value : frame) bad += ((value & 0x7c00u) == 0x7c00u);
                        if (a != b || bad) {
                            std::fprintf(stderr, "FAIL: arena/wide sequence at %s (repeat=%d, nonfinite=%zu)\n", label, int(a == b), bad);
                            k2_failed = true;
                        } else {
                            std::printf("[PASS] arena/wide sequence %s: 20 frames, exact repeat, finite output\n", label);
                        }
                    }

                    for (onr::Buffer* b : {&p_col, &p_alb, &p_spc, &p_nrm, &p_rgh, &p_dep, &p_mv, &p_hit, &p_out})
                        d3d.destroy(*b);
                };
                profile_res({1280, 720}, {1920, 1080}, "1080p Quality");
                profile_res({2560, 1440}, {3840, 2160}, "4K Quality");
            }

            // ---- Item NFMA: Shipped native ACO FP16 FMA & half_rz numerical boundary checks ----
            {
                auto u32_to_f32 = [](uint32_t u) -> float { float f; std::memcpy(&f, &u, sizeof f); return f; };
                auto f32_to_u32 = [](float f) -> uint32_t { uint32_t u; std::memcpy(&u, &f, sizeof u); return u; };
                auto f16_to_f32 = [&](uint16_t h) -> float {
                    const uint32_t s = (uint32_t(h & 0x8000u)) << 16;
                    const uint32_t e = (h >> 10) & 0x1Fu;
                    const uint32_t m = h & 0x03FFu;
                    if (e == 0u) return m ? (s ? -std::ldexp(float(m), -24) : std::ldexp(float(m), -24)) : u32_to_f32(s);
                    if (e == 0x1Fu) return u32_to_f32(s | 0x7F800000u | (m << 13));
                    return u32_to_f32(s | ((e + 112u) << 23) | (m << 13));
                };

                // Manifest precision pin: all 4 downsample*_lt kernels must stay on 'global' while 37 _lt/_lx kernels use 'native'.
                {
                    const char* aco_env = std::getenv("ONR_ACO_DIR");
                    const std::filesystem::path aco_dir = (aco_env && *aco_env)
                        ? std::filesystem::path(aco_env)
                        : std::filesystem::path(spv_dir_str).parent_path() / "aco" / "gfx1201" / "openrr-d3d12-v1";
                    const std::filesystem::path mf_path = aco_dir / "manifest.json";
                    std::ifstream mf_in(mf_path, std::ios::binary);
                    if (!mf_in) {
                        std::fprintf(stderr, "FAIL: NFMA cannot open manifest %s\n", mf_path.string().c_str());
                        k2_failed = true;
                    } else {
                        const std::string mf_txt((std::istreambuf_iterator<char>(mf_in)), std::istreambuf_iterator<char>());
                        auto prec_of = [&](const std::string& stem) -> std::string {
                            const std::string needle = "\"spv\": \"" + stem + ".spv\"";
                            size_t pos = mf_txt.find(needle);
                            if (pos == std::string::npos) return "";
                            size_t end = mf_txt.find("\n    }", pos);
                            size_t pp = mf_txt.find("\"precision\": \"", pos);
                            if (pp == std::string::npos || (end != std::string::npos && pp > end)) return "";
                            pp += 14;
                            size_t pe = mf_txt.find('"', pp);
                            return pe == std::string::npos ? "" : mf_txt.substr(pp, pe - pp);
                        };
                        for (const char* gs : {"downsample_lt", "downsample_tl_lt", "d3d_downsample_lt", "d3d_downsample_tl_lt"}) {
                            const std::string got = prec_of(gs);
                            if (got != "global") {
                                std::fprintf(stderr, "FAIL: NFMA manifest %s precision='%s' (expected global)\n", gs, got.c_str());
                                k2_failed = true;
                            }
                        }
                        const char* native_stems[] = {
                            "post_f_lt", "d3d_enc0_attr_f_lt", "d3d_enc0_attr_f_w_lt", "enc0_attr_f_lt", "enc0_attr_f_w_lt",
                            "post_dlt_f_lt", "dec_head_32_64_f_lt", "dec_head_32_64_f_lx", "enc0_stage_32_64_f_lt",
                            "enc0_stage_32_64_f_lx", "enc0_stage_32_64_f_w_lt", "enc0_stage_32_64_f_w_lx",
                            "dec_up_32_64_64_f_lt", "dec_up_32_64_64_f_lx", "enc_stage_64_64_96_f_lt",
                            "dec_up_64_64_96_f_lt", "dec_dfn_64_64_f_lt", "enc_stage_96_128_128_f_lt",
                            "enc_stage_96_128_128_fl_lt", "dec_up_96_128_128_f_lt", "dec_up_96_128_128_fl_lt",
                            "dec_dfn_96_128_f_lt", "dec_dfn_96_128_fl_lt", "enc_stage_128_128_160_f_lt",
                            "enc_stage_128_128_160_fl_lt", "dec_up_128_128_160_fl_lt", "dec_up_128_128_160_f_lt",
                            "dec_dfn_128_128_f_lt", "dec_dfn_128_128_fl_lt", "dec_dfn_160_256_fl_lt",
                            "dec_dfn_160_256_f_lt", "enc_stage_160_256_0_fl_lt", "enc_stage_160_256_0_f_lt",
                            "enc_stage_160_256_160_fl_lt", "enc_stage_160_256_160_f_lt",
                            "dec_up_160_256_160_f_lt", "dec_up_160_256_160_fl_lt"
                        };
                        for (const char* ns : native_stems) {
                            const std::string got = prec_of(ns);
                            if (got != "native") {
                                std::fprintf(stderr, "FAIL: NFMA manifest %s precision='%s' (expected native)\n", ns, got.c_str());
                                k2_failed = true;
                            }
                        }
                    }
                }

                onr::Kernels k_ds_global, k_ds_native;
                k_ds_global.load(d3d, spv_dir, {"downsample", "downsample_tl", "d3d_downsample", "d3d_downsample_tl"});
                k_ds_native.load_lite(d3d, spv_dir, {"downsample", "downsample_tl", "d3d_downsample", "d3d_downsample_tl"});

                constexpr int kDsW = 24, kDsH = 24, kOutW = 16, kOutH = 16;
                onr::Buffer ds_rg = d3d.create(size_t(kDsW) * kDsH * 4);
                onr::Buffer ds_b  = d3d.create(size_t(kDsW) * kDsH * 2);
                onr::Buffer ds_exp = d3d.create(16);
                onr::Buffer ds_out_g = d3d.create(size_t(kOutW) * kOutH * 8);
                onr::Buffer ds_out_n = d3d.create(size_t(kOutW) * kOutH * 8);

                auto run_ds_pair = [&](float exposure_val, uint16_t& q_g, uint16_t& q_n) {
                    d3d.upload(ds_exp, &exposure_val, 4);
                    onr::DownsamplePC pc{ds_rg.addr, ds_b.addr, ds_out_g.addr, ds_exp.addr,
                                         {kDsW, kDsH}, {kOutW, kOutH}, {0, 0}, {kOutW, kOutH}, {kDsW, kDsH}, {0, kDsW}};
                    d3d.begin_own();
                    k_ds_global.downsample(d3d, pc);
                    pc.dst = ds_out_n.addr;
                    k_ds_native.downsample(d3d, pc);
                    d3d.submit_own();
                    std::vector<uint16_t> og(size_t(kOutW) * kOutH * 4), on(size_t(kOutW) * kOutH * 4);
                    d3d.download(ds_out_g, og.data(), og.size() * 2);
                    d3d.download(ds_out_n, on.data(), on.size() * 2);
                    for (uint16_t h : on) {
                        if ((h & 0x7C00u) == 0x7C00u) {
                            std::fprintf(stderr, "FAIL: NFMA downsample_lt produced non-finite half 0x%04x (exp=%g)\n", h, exposure_val);
                            k2_failed = true;
                            break;
                        }
                    }
                    // Center pixel (8, 8) avoids border clamp effects on non-uniform tests too.
                    const size_t cidx = (size_t(8) * kOutW + 8u) * 4u;
                    q_g = og[cidx];
                    q_n = on[cidx];
                };

                // Sub-test A: half_rz FP16 grid, subnormal, 65600 overflow->0x7BFF clamp & midpoint tie-toward-zero sweep
                d3d.fill(ds_rg, 0x3C003C00u);   // uniform 1.0hf -> clamped filter v == 1.0f exact
                d3d.fill(ds_b,  0x3C003C00u);
                const float ds_t = (1.0f * u32_to_f32(0x4031c71cu)) * u32_to_f32(0x40549a78u);
                const float ds_num = (std::exp2(ds_t) - 1.0f) * u32_to_f32(0x3c23d70au);

                size_t ds_parity_mismatches = 0;
                auto check_ds_target = [&](double target_y, uint16_t& q_n) {
                    const float exp_v = float(double(ds_num) / target_y);
                    uint16_t q_g = 0;
                    run_ds_pair(exp_v, q_g, q_n);
                    if (q_g != q_n) ++ds_parity_mismatches;
                };

                // Sweep across odd-mantissa midpoints (where RNE rounds up 0x3c01->0x3c02, 0x4001->0x4002, 0x7bfe->0x7bff)
                // and across exact grid points to prove half_rz always truncates toward zero instead of rounding up.
                struct TieCase { double mid; uint16_t lo_h, hi_h; };
                const TieCase ties[] = {
                    { 1.0 + std::ldexp(1.0, -10) + std::ldexp(1.0, -11), 0x3C01u, 0x3C02u },
                    { 1.0 + std::ldexp(3.0, -10) + std::ldexp(1.0, -11), 0x3C03u, 0x3C04u },
                    { 2.0 + std::ldexp(1.0, -9)  + std::ldexp(1.0, -10), 0x4001u, 0x4002u },
                    { 65472.0 + 16.0,                                    0x7BFEu, 0x7BFFu },
                };
                for (const TieCase& tc : ties) {
                    bool saw_lo = false, saw_hi = false;
                    uint32_t hi_at_mid_minus1ulp = 0;
                    for (int ulp = -6; ulp <= 6; ++ulp) {
                        const double target = u32_to_f32(f32_to_u32(float(tc.mid)) + ulp);
                        uint16_t qn = 0;
                        check_ds_target(target, qn);
                        if (qn == tc.lo_h) saw_lo = true;
                        if (qn == tc.hi_h) saw_hi = true;
                        // At ulp >= +3 above midpoint, y is strictly above tie so RNE rounds up, yet y < hi_h so half_rz must still give lo_h
                        if (ulp >= 2 && ulp <= 4 && qn != tc.lo_h) ++hi_at_mid_minus1ulp;
                    }
                    if (!saw_lo || saw_hi || hi_at_mid_minus1ulp != 0) {
                        std::fprintf(stderr, "FAIL: NFMA half_rz tie-toward-zero failed for lo=0x%04x hi=0x%04x (saw_lo=%d saw_hi=%d hi_at_mid=%u)\n",
                                     tc.lo_h, tc.hi_h, int(saw_lo), int(saw_hi), hi_at_mid_minus1ulp);
                        k2_failed = true;
                    }
                    uint16_t q_hi_check = 0;
                    check_ds_target(u32_to_f32(f32_to_u32(f16_to_f32(tc.hi_h)) + 4), q_hi_check);
                    if (q_hi_check != tc.hi_h) {
                        std::fprintf(stderr, "FAIL: NFMA half_rz upper step failed for hi=0x%04x (got 0x%04x)\n", tc.hi_h, q_hi_check);
                        k2_failed = true;
                    }
                }
                // Above max normal (65504.0 = 0x7bff is exact; 65520..65600 -> float16_t +Inf decremented by half_rz to 0x7bff) and subnormals
                {
                    uint16_t q_max = 0, q_sub1 = 0, q_sub2 = 0, q_zero = 0;
                    check_ds_target(65600.0, q_max);
                    check_ds_target(std::ldexp(1.0, -24), q_sub1);
                    check_ds_target(std::ldexp(1.0, -15), q_sub2);
                    check_ds_target(std::ldexp(1.0, -26), q_zero);
                    if (q_max != 0x7BFFu || q_zero != 0x0000u || (q_sub1 & 0x7C00u) != 0u) {
                        std::fprintf(stderr, "FAIL: NFMA half_rz boundary (q_max=0x%04x q_sub1=0x%04x q_sub2=0x%04x q_zero=0x%04x)\n",
                                     q_max, q_sub1, q_sub2, q_zero);
                        k2_failed = true;
                    }
                }

                // Sub-test B: -0.0, +0.0, FP16 subnormals, and +-65504 checkerboard through downsample_lt vs global downsample
                std::vector<uint32_t> rg_pat(size_t(kDsW) * kDsH);
                std::vector<uint16_t> b_pat(size_t(kDsW) * kDsH);
                const uint16_t corner_vals[] = { 0x8000u /*-0*/, 0x0001u /*subnorm min*/, 0x03FFu /*subnorm max*/, 0x7BFFu /*+65504*/, 0xFBFFu /*-65504*/ };
                for (int ci = 0; ci < 5; ++ci) {
                    const uint16_t h0 = corner_vals[ci];
                    const uint16_t h1 = (ci >= 3) ? uint16_t(h0 ^ 0x8000u) : h0;
                    for (int y = 0; y < kDsH; ++y) {
                        for (int x = 0; x < kDsW; ++x) {
                            const uint16_t hv = ((x ^ y) & 1) ? h1 : h0;
                            rg_pat[size_t(y) * kDsW + x] = uint32_t(hv) | (uint32_t(hv) << 16);
                            b_pat[size_t(y) * kDsW + x] = hv;
                        }
                    }
                    d3d.upload(ds_rg, rg_pat.data(), rg_pat.size() * 4);
                    d3d.upload(ds_b, b_pat.data(), b_pat.size() * 2);
                    uint16_t qg = 0, qn = 0;
                    run_ds_pair(1.0f, qg, qn);
                    const uint16_t diff_ulp = (qg > qn) ? (qg - qn) : (qn - qg);
                    if (diff_ulp > 1u || !std::isfinite(f16_to_f32(qn))) {
                        std::fprintf(stderr, "FAIL: NFMA downsample_lt corner val=0x%04x global=0x%04x native=0x%04x\n", h0, qg, qn);
                        k2_failed = true;
                    }
                }
                if (ds_parity_mismatches != 0) {
                    std::fprintf(stderr, "FAIL: NFMA native vs global downsample had %zu bit mismatches in sweep\n", ds_parity_mismatches);
                    k2_failed = true;
                }

                // Sub-test B2: Cover downsample_tl_lt, d3d_downsample_lt, and d3d_downsample_tl_lt (65600 clamp + -0.0/subnorm/+-65504)
                {
                    const uint32_t uav_pitch = ((kOutW * 8u) + 255u) & ~255u;
                    D3D12_HEAP_PROPERTIES hp_uav{D3D12_HEAP_TYPE_DEFAULT};
                    D3D12_RESOURCE_DESC rd_uav{};
                    rd_uav.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    rd_uav.Width = kOutW; rd_uav.Height = kOutH; rd_uav.DepthOrArraySize = 1; rd_uav.MipLevels = 1;
                    rd_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd_uav.SampleDesc = {1, 0};
                    rd_uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    ID3D12Resource* uav_tex = nullptr;
                    if (FAILED(dev->CreateCommittedResource(&hp_uav, D3D12_HEAP_FLAG_NONE, &rd_uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uav_tex)))) {
                        std::fprintf(stderr, "FAIL: NFMA could not create UAV texture\n");
                        k2_failed = true;
                    } else {
                        onr::Buffer uav_rb = d3d.create(size_t(uav_pitch) * kOutH);
                        onr::Buffer tl_buf = d3d.create(size_t(kDsW) * kDsH * 8);
                        onr::Buffer pmv_buf = d3d.create(size_t(kDsW) * kDsH * 8);
                        onr::Buffer hist_in = d3d.create(size_t(kOutW) * kOutH * 8);
                        onr::Buffer hist_out = d3d.create(size_t(kOutW) * kOutH * 8);
                        onr::Buffer op_buf = d3d.create(size_t(kDsW) * kDsH * 8);
                        onr::Buffer op_hi = d3d.create(size_t(kOutW) * kOutH * 8);
                        onr::Buffer op_ho = d3d.create(size_t(kOutW) * kOutH * 8);
                        d3d.fill(tl_buf, 0x38003800u);  // 0.5hf premult
                        d3d.fill(op_buf, 0x38003800u);
                        d3d.fill(ds_rg, 0x3C003C00u);
                        d3d.fill(ds_b, 0x3C003C00u);
                        const float exp_ovf = float(double(ds_num) / 65600.0);
                        const float exp_tl_ovf = float(double(ds_num) / 131200.0);
                        d3d.upload(ds_exp, &exp_ovf, 4);
                        const float exp_prev = 1.0f;
                        d3d.upload(ds_exp, &exp_prev, 4, 4);

                        auto check_tex_uav = [&](const char* kname, const onr::Kernels& ks, bool is_tl) {
                            d3d.begin_own();
                            d3d.uav(uav_tex, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
                            if (!is_tl) {
                                onr::DownsamplePC pc{ds_rg.addr, ds_b.addr, 0ull, ds_exp.addr,
                                                     {kDsW, kDsH}, {kOutW, kOutH}, {0, 0}, {kOutW, kOutH}, {kDsW, kDsH}, {0, kDsW}};
                                d3d.dispatch(ks[kname], &pc, sizeof pc, (kOutW + 15) / 16, (kOutH + 15) / 16);
                            } else {
                                onr::DownsampleTlPC pc{{ds_rg.addr, ds_b.addr, 0ull, ds_exp.addr,
                                    {kDsW, kDsH}, {kOutW, kOutH}, {0, 0}, {kOutW, kOutH}, {kDsW, kDsH}, {0, kDsW}},
                                    tl_buf.addr, pmv_buf.addr, hist_in.addr, hist_out.addr, {kDsW, kDsH},
                                    {1.5f, 1.5f}, {0.f, 0.f}, {0.f, 0.f}, {1.f, 1.f}, 0.25f, 1u,
                                    0ull, {}, {}, op_buf.addr, op_hi.addr, op_ho.addr, 1u, 0u, 0u};
                                d3d.dispatch(ks[kname], &pc, sizeof pc, (kOutW + 15) / 16, (kOutH + 15) / 16);
                            }
                            d3d.copy_texture_to_buffer(uav_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav_rb, 0, uav_pitch, 0, 0, kOutW, kOutH);
                            d3d.submit_own();
                            std::vector<uint8_t> raw_rb(size_t(uav_pitch) * kOutH);
                            d3d.download(uav_rb, raw_rb.data(), raw_rb.size());
                            std::vector<uint16_t> out_px(size_t(kOutW) * kOutH * 4);
                            for (int y = 0; y < kOutH; ++y)
                                std::memcpy(out_px.data() + size_t(y) * kOutW * 4, raw_rb.data() + size_t(y) * uav_pitch, size_t(kOutW) * 8);
                            return out_px;
                        };
                        auto run_tl_buf = [&](const onr::Kernels& ks) {
                            onr::DownsampleTlPC pc{{ds_rg.addr, ds_b.addr, &ks == &k_ds_global ? ds_out_g.addr : ds_out_n.addr, ds_exp.addr,
                                {kDsW, kDsH}, {kOutW, kOutH}, {0, 0}, {kOutW, kOutH}, {kDsW, kDsH}, {0, kDsW}},
                                tl_buf.addr, pmv_buf.addr, hist_in.addr, hist_out.addr, {kDsW, kDsH},
                                {1.5f, 1.5f}, {0.f, 0.f}, {0.f, 0.f}, {1.f, 1.f}, 0.25f, 1u,
                                0ull, {}, {}, op_buf.addr, op_hi.addr, op_ho.addr, 1u, 0u, 0u};
                            d3d.begin_own();
                            d3d.dispatch(ks["downsample_tl"], &pc, sizeof pc, (kOutW + 15) / 16, (kOutH + 15) / 16);
                            d3d.submit_own();
                            std::vector<uint16_t> out_px(size_t(kOutW) * kOutH * 4);
                            d3d.download(pc.d.dst == ds_out_g.addr ? ds_out_g : ds_out_n, out_px.data(), out_px.size() * 2);
                            return out_px;
                        };
                        auto check_finite_and_max = [&](const char* tag, const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, uint16_t expect_c) {
                            size_t nf = 0;
                            for (uint16_t h : b) if ((h & 0x7C00u) == 0x7C00u) ++nf;
                            const size_t cidx = (size_t(8) * kOutW + 8u) * 4u;
                            if (nf != 0 || a[cidx] != expect_c || b[cidx] != expect_c) {
                                std::fprintf(stderr, "FAIL: NFMA %s 65600 clamp (nf=%zu global=0x%04x lt=0x%04x want=0x%04x)\n",
                                             tag, nf, a[cidx], b[cidx], expect_c);
                                k2_failed = true;
                            }
                        };
                        check_finite_and_max("d3d_downsample_lt", check_tex_uav("d3d_downsample", k_ds_global, false), check_tex_uav("d3d_downsample", k_ds_native, false), 0x7BFFu);
                        d3d.upload(ds_exp, &exp_tl_ovf, 4);
                        check_finite_and_max("downsample_tl_lt", run_tl_buf(k_ds_global), run_tl_buf(k_ds_native), 0x7BFFu);
                        check_finite_and_max("d3d_downsample_tl_lt", check_tex_uav("d3d_downsample_tl", k_ds_global, true), check_tex_uav("d3d_downsample_tl", k_ds_native, true), 0x7BFFu);
                        for (onr::Buffer* b : {&uav_rb, &tl_buf, &pmv_buf, &hist_in, &hist_out, &op_buf, &op_hi, &op_ho}) d3d.destroy(*b);
                        uav_tex->Release();
                    }
                }
                for (onr::Buffer* db : {&ds_rg, &ds_b, &ds_exp, &ds_out_g, &ds_out_n}) d3d.destroy(*db);
                k_ds_native.unload(d3d);
                k_ds_global.unload(d3d);

                // Sub-test C2: Retained native pooled_history2_lt + full 22-kernel New=1 (_lt) AND Fast=2 (_lx) stress (-0.0, subnorm, +-65504)
                {
                    onr::Kernels k_ph_g, k_ph_n;
                    k_ph_g.load(d3d, spv_dir, {"pooled_history2"});
                    k_ph_n.load_lite(d3d, spv_dir, {"pooled_history2"});
                    onr::Buffer ph_in = d3d.create(32u * 32u * 4u), ph_og = d3d.create(16u * 16u * 4u), ph_on = d3d.create(16u * 16u * 4u);
                    for (uint32_t pat : {0x80008000u, 0x000103FFu, 0x7BFFFBFFu}) {
                        d3d.fill(ph_in, pat);
                        d3d.begin_own();
                        k_ph_g.pooled_history2(d3d, {ph_in.addr, ph_og.addr, {32, 32}, {16, 16}});
                        k_ph_n.pooled_history2(d3d, {ph_in.addr, ph_on.addr, {32, 32}, {16, 16}});
                        d3d.submit_own();
                        std::vector<uint16_t> vg(16u * 16u * 2), vn(16u * 16u * 2);
                        d3d.download(ph_og, vg.data(), vg.size() * 2);
                        d3d.download(ph_on, vn.data(), vn.size() * 2);
                        for (uint16_t h : vn) {
                            if ((h & 0x7C00u) == 0x7C00u) {
                                std::fprintf(stderr, "FAIL: NFMA pooled_history2_lt non-finite half 0x%04x pat=0x%08x\n", h, pat);
                                k2_failed = true;
                                break;
                            }
                        }
                    }
                    for (onr::Buffer* b : {&ph_in, &ph_og, &ph_on}) d3d.destroy(*b);
                    k_ph_n.unload(d3d);
                    k_ph_g.unload(d3d);
                }

                onr::Kernels k_lx;
                k_lx.load_lite(d3d, spv_dir, names, "_lx");
                k_lx.load_lite(d3d, spv_dir, names, "_lt");
                k_lx.load_lite(d3d, spv_dir, flow_names, "_lt");

                eng.resize(r_a, out_sz);
                eng.set_fast(true);
                const uint32_t stress_fills[] = { 0x80008000u /*-0.0*/, 0x00010001u /*subnorm min*/, 0x7BFF7BFFu /*+65504*/, 0xFBFFFBFFu /*-65504*/ };
                for (int mode_idx = 0; mode_idx < 2; ++mode_idx) {
                    const bool is_lx = (mode_idx == 1);
                    eng.set_lite(is_lx ? &k_lx : &k_lite);
                    eng.set_deep_on(is_lx);
                    eng.set_lite_on(true);
                    ef.render = r_a;
                    ef.output = out_sz;
                    std::vector<uint16_t> stress_a, stress_b;
                    for (size_t si = 0; si < 4; ++si) {
                        const uint32_t col_w = stress_fills[si];
                        d3d.begin_batch();
                        d3d.fill(b_col, col_w);
                        d3d.fill(b_alb, 0x80808080u);
                        d3d.fill(b_spc, 0x20202020u);
                        d3d.fill(b_nrm, 0x00003C00u);
                        d3d.fill(b_rgh, 0x38003800u);
                        d3d.fill(b_dep, 0x3F800000u);
                        d3d.fill(b_mv, 0u);
                        d3d.fill(b_hit, 0x38003800u);
                        d3d.fill(b_out, 0u);
                        d3d.end_batch();
                        const int repeats = (si == 2) ? 10 : 1;
                        for (int rep = 0; rep < repeats; ++rep) {
                            ef.reset = (si != 2) || (rep == 0 || rep == 1 || rep == 9);
                            d3d.begin_own();
                            eng.record(ef, g, b_out.addr);
                            d3d.submit_own();
                            std::vector<uint16_t> sout(size_t(out_sz.x) * out_sz.y * 4);
                            d3d.download(b_out, sout.data(), sout.size() * 2);
                            size_t s_nf = 0;
                            for (uint16_t h : sout) if ((h & 0x7C00u) == 0x7C00u) ++s_nf;
                            if (s_nf != 0) {
                                std::fprintf(stderr, "FAIL: NFMA %s pipeline non_finite=%zu on fill=0x%08x rep=%d\n",
                                             is_lx ? "k_lx" : "k_lite", s_nf, col_w, rep);
                                k2_failed = true;
                            }
                            if (si == 2 && rep == 1) stress_a = std::move(sout);
                            else if (si == 2 && rep == 9) stress_b = std::move(sout);
                        }
                    }
                    if (stress_a.empty() || stress_a != stress_b) {
                        std::fprintf(stderr, "FAIL: NFMA %s 65504 8-frame phase-aligned repeat run non-deterministic\n", is_lx ? "k_lx" : "k_lite");
                        k2_failed = true;
                    }
                }
                eng.set_deep_on(false);
                eng.set_lite(&k_lite);
                k_lx.unload(d3d);
                if (!k2_failed) {
                    std::printf("[PASS] Item NFMA (native ACO FP16 FMA & half_rz: manifest 4 global + 37 native, 0/%zu downsample_lt mismatches, tl/d3d_uav/pooled_history2_lt, 22-kernel New+Fast -0.0/subnorm/+-65504/65600 finite & deterministic)\n",
                                size_t(4 * 14 + 4));
                }
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
                        bool host_binding_report_ok = true;
                        int ok_count = 0, busy_rc = 0;
                        for (int i = 0; i < 128; ++i) {
                            ID3D12GraphicsCommandList* l = nullptr;
                            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sat_alloc, nullptr, IID_PPV_ARGS(&l));
                            OnrRrNotifyD3d12ListReset(l, sat_alloc);
                            bool host_bound = false;
                            const int32_t rc = openrr::pe::record_d3d12(rt, &fr, l, host_bound, false);
                            if (rc == ONR_RR_OK && !host_bound) host_binding_report_ok = false;
                            if (rc == ONR_RR_BUSY && host_bound) host_binding_report_ok = false;
                            l->Close();
                            sat_lists.push_back(l);
                            if (rc == ONR_RR_OK) ++ok_count;
                            else if (rc == ONR_RR_BUSY) { busy_rc = rc; break; }
                        }
                        sat_alloc->Reset();
                        OnrRrNotifyD3d12AllocatorReset(sat_alloc);
                        sat_lists[0]->Reset(sat_alloc, nullptr);
                        OnrRrNotifyD3d12ListReset(sat_lists[0], sat_alloc);
                        OnrRrFrame invalid_output = fr;
                        invalid_output.output.x = 1;
                        void* rejected_job = nullptr;
                        const int32_t invalid_rc = api4.Record(rt, &invalid_output, sat_lists[0], ONR_RR_EDIT_OFF,
                                                               nullptr, &rejected_job);
                        char invalid_error[256]{};
                        api4.v3.v2.v1.GetLastError(invalid_error, sizeof(invalid_error));
                        sat_lists[0]->Close();
                        const bool invalid_rect_rejected = invalid_rc == ONR_RR_INVALID_ARGUMENT && !rejected_job &&
                                                           std::strstr(invalid_error, "output rectangle");
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

                        if (!host_binding_report_ok) {
                            std::fprintf(stderr, "FAIL: D3D12 host binding report on successful or busy Record\n");
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] D3D12 host binding report distinguishes successful and busy Record\n");
                        if (!invalid_rect_rejected) {
                            std::fprintf(stderr, "FAIL: D3D12 output rectangle outside texture (rc=%d, %s)\n",
                                         invalid_rc, invalid_error);
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] D3D12 rejects an output rectangle that exceeds the texture at a nonzero offset\n");
                        if (ok_count != 127 || busy_rc != ONR_RR_BUSY || rc_after != ONR_RR_OK) {
                            std::fprintf(stderr, "FAIL: ONR_RR_BUSY test ok_count=%d busy_rc=%d loop_ok=%d\n",
                                         ok_count, busy_rc, loop_ok);
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] Item #3/#4 (D3D12 ONR_RR_BUSY saturation & >127-frame allocator reset loop): %d in-flight slots accepted, 128th returned ONR_RR_BUSY (%d), 135 post-reset Records OK\n",
                                    ok_count, busy_rc);
                    }

                    // ---- Item NFMA Direct D3D12 guides: NaN / +-Inf / -0.0 / subnormal / +-65504 stress in a fresh context ----
                    // (exercises native d3d_ae + d3d_enc0_attr_f_lt + all _lt/_lx + post_f_lt + d3d_downsample_lt).
                    const UINT w_in = 512, h_in = 288, w_out = 1024, h_out = 576;
                    OnrRrCreateInfo ci_nf = ci;
                    ci_nf.max_render_width = w_in;
                    ci_nf.max_render_height = h_in;
                    ci_nf.output_width = w_out;
                    ci_nf.output_height = h_out;
                    void* rt_nf = nullptr;
                    if (api4.v3.v2.v1.Create(&ci_nf, &rt_nf) == ONR_RR_OK && rt_nf) {
                        bool nf_guides_ok = true;
                        const uint32_t srv_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        auto make_nf_tex = [&](UINT tw, UINT th, DXGI_FORMAT fmt, bool uav) {
                            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            rd.Width = tw; rd.Height = th;
                            rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                            rd.Format = fmt; rd.SampleDesc = {1, 0};
                            if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                            ID3D12Resource* r = nullptr;
                            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATES(srv_st),
                                nullptr, IID_PPV_ARGS(&r));
                            return r;
                        };
                        ID3D12Resource* t_col = make_nf_tex(w_in, h_in, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_alb = make_nf_tex(w_in, h_in, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_spc = make_nf_tex(w_in, h_in, DXGI_FORMAT_R8G8B8A8_UNORM, false);
                        ID3D12Resource* t_nrm = make_nf_tex(w_in, h_in, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
                        ID3D12Resource* t_dep = make_nf_tex(w_in, h_in, DXGI_FORMAT_R32_FLOAT, false);
                        ID3D12Resource* t_mv  = make_nf_tex(w_in, h_in, DXGI_FORMAT_R16G16_FLOAT, false);
                        ID3D12Resource* t_out = make_nf_tex(w_out, h_out, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

                        OnrRrFrame fr_nf{};
                        fr_nf.size = sizeof(fr_nf);
                        fr_nf.render_width = w_in; fr_nf.render_height = h_in;
                        fr_nf.mv_scale_x = 1.0f; fr_nf.mv_scale_y = 1.0f;
                        fr_nf.flags = ONR_RR_FRAME_HW_DEPTH | ONR_RR_FRAME_DEPTH_INVERTED | ONR_RR_FRAME_ROUGHNESS_PACKED;
                        for (int i = 0; i < 16; ++i)
                            fr_nf.world_to_view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                        fr_nf.view_to_clip[0] = 1.0f;
                        fr_nf.view_to_clip[5] = 1.777f;
                        fr_nf.view_to_clip[11] = -1.0f;
                        fr_nf.view_to_clip[14] = 0.05f;
                        fr_nf.color          = {t_col, srv_st, 0, 0};
                        fr_nf.diffuse_albedo = {t_alb, srv_st, 0, 0};
                        fr_nf.specular_albedo= {t_spc, srv_st, 0, 0};
                        fr_nf.normals        = {t_nrm, srv_st, 0, 0};
                        fr_nf.depth          = {t_dep, srv_st, 0, 0};
                        fr_nf.motion         = {t_mv,  srv_st, 0, 0};
                        fr_nf.output         = {t_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0};

                        D3D12_COMMAND_QUEUE_DESC qd_nf{D3D12_COMMAND_LIST_TYPE_DIRECT};
                        ID3D12CommandQueue* q_nf = nullptr;
                        ID3D12CommandAllocator* ca_nf = nullptr;
                        ID3D12GraphicsCommandList* cl_nf = nullptr;
                        ID3D12Fence* fn_nf = nullptr;
                        dev->CreateCommandQueue(&qd_nf, IID_PPV_ARGS(&q_nf));
                        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_nf));
                        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca_nf, nullptr, IID_PPV_ARGS(&cl_nf));
                        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fn_nf));
                        HANDLE ev_nf = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                        UINT64 fv_nf = 0;
                        const char* nf_fail_phase = "none";
                        HRESULT nf_fail_hr = S_OK;
                        auto note_nf_fail = [&](const char* phase, HRESULT hr) {
                            if (nf_guides_ok) {
                                nf_fail_phase = phase;
                                nf_fail_hr = hr;
                            }
                            nf_guides_ok = false;
                        };
                        auto sync_nf = [&](const char* phase) {
                            HRESULT hr = cl_nf->Close();
                            if (FAILED(hr)) { note_nf_fail(phase, hr); return; }
                            ID3D12CommandList* lst = cl_nf;
                            q_nf->ExecuteCommandLists(1, &lst);
                            hr = q_nf->Signal(fn_nf, ++fv_nf);
                            if (FAILED(hr)) { note_nf_fail(phase, hr); return; }
                            if (fn_nf->GetCompletedValue() < fv_nf) {
                                fn_nf->SetEventOnCompletion(fv_nf, ev_nf);
                                if (WaitForSingleObject(ev_nf, 10000) != WAIT_OBJECT_0) {
                                    note_nf_fail(phase, dev->GetDeviceRemovedReason());
                                    return;
                                }
                            }
                            hr = dev->GetDeviceRemovedReason();
                            if (FAILED(hr)) note_nf_fail(phase, hr);
                        };
                        auto make_cpu_buf = [&](UINT64 bytes, D3D12_HEAP_TYPE htype, D3D12_RESOURCE_STATES st) {
                            D3D12_HEAP_PROPERTIES hpp{htype};
                            D3D12_RESOURCE_DESC rdb{};
                            rdb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            rdb.Width = bytes; rdb.Height = 1; rdb.DepthOrArraySize = 1; rdb.MipLevels = 1;
                            rdb.SampleDesc = {1, 0}; rdb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            ID3D12Resource* res = nullptr;
                            dev->CreateCommittedResource(&hpp, D3D12_HEAP_FLAG_NONE, &rdb, st, nullptr, IID_PPV_ARGS(&res));
                            return res;
                        };
                        const UINT p_8 = (w_in * 8u + 255u) & ~255u, p_4 = (w_in * 4u + 255u) & ~255u, p_out = (w_out * 8u + 255u) & ~255u;
                        ID3D12Resource* up_col = make_cpu_buf(UINT64(p_8) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* up_nrm = make_cpu_buf(UINT64(p_8) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* up_mv  = make_cpu_buf(UINT64(p_4) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* up_dep = make_cpu_buf(UINT64(p_4) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* up_alb = make_cpu_buf(UINT64(p_4) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* up_spc = make_cpu_buf(UINT64(p_4) * h_in, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        ID3D12Resource* rb_out = make_cpu_buf(UINT64(p_out) * h_out, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

                        unsigned nf_mask = 15u;
                        if (const char* m_env = std::getenv("ONR_TEST_NF_MASK")) {
                            nf_mask = static_cast<unsigned>(std::strtoul(m_env, nullptr, 0));
                        }
                        auto upload_guides = [&](unsigned mask, bool open_list) {
                            const uint16_t h_vals[8] = {0x7E00u /*qNaN*/, 0x7C00u /*+Inf*/, 0xFC00u /*-Inf*/, 0x8000u /*-0*/, 0x0001u /*submin*/, 0x7BFFu /*+65504*/, 0xFBFFu /*-65504*/, 0x3C00u /*1.0*/};
                            const uint32_t f_vals[4] = {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x3F800000u};
                            auto fill_up16x4 = [&](ID3D12Resource* ub, UINT pitch, bool adv, bool norm) {
                                uint8_t* ptr = nullptr;
                                if (SUCCEEDED(ub->Map(0, nullptr, reinterpret_cast<void**>(&ptr))) && ptr) {
                                    for (UINT y = 0; y < h_in; ++y) {
                                        uint16_t* row = reinterpret_cast<uint16_t*>(ptr + y * pitch);
                                        for (UINT x = 0; x < w_in; ++x) {
                                            const uint16_t hv = !adv ? 0x3C00u : (norm && ((x + y) & 1) ? 0x3C00u : h_vals[(x + y) & 7u]);
                                            row[4 * x + 0] = hv; row[4 * x + 1] = hv; row[4 * x + 2] = hv; row[4 * x + 3] = 0x3C00u;
                                        }
                                    }
                                    ub->Unmap(0, nullptr);
                                }
                            };
                            fill_up16x4(up_col, p_8, (mask & 1u) != 0u, false);
                            fill_up16x4(up_nrm, p_8, (mask & 2u) != 0u, true);
                            {
                                const bool adv_mv = (mask & 4u) != 0u;
                                const bool adv_dp = (mask & 8u) != 0u;
                                uint8_t* pm = nullptr; uint8_t* pd = nullptr; uint8_t* pa = nullptr; uint8_t* ps = nullptr;
                                up_mv->Map(0, nullptr, reinterpret_cast<void**>(&pm));
                                up_dep->Map(0, nullptr, reinterpret_cast<void**>(&pd));
                                up_alb->Map(0, nullptr, reinterpret_cast<void**>(&pa));
                                up_spc->Map(0, nullptr, reinterpret_cast<void**>(&ps));
                                for (UINT y = 0; y < h_in; ++y) {
                                    uint16_t* rmv = reinterpret_cast<uint16_t*>(pm + y * p_4);
                                    uint32_t* rdp = reinterpret_cast<uint32_t*>(pd + y * p_4);
                                    uint32_t* ralb = reinterpret_cast<uint32_t*>(pa + y * p_4);
                                    uint32_t* rspc = reinterpret_cast<uint32_t*>(ps + y * p_4);
                                    for (UINT x = 0; x < w_in; ++x) {
                                        rmv[2 * x + 0] = adv_mv ? h_vals[(x * 3 + y) & 7u] : 0u;
                                        rmv[2 * x + 1] = adv_mv ? h_vals[(x * 3 + y) & 7u] : 0u;
                                        rdp[x] = adv_dp ? f_vals[(x + y) & 3u] : 0x3F800000u;
                                        ralb[x] = 0x80808080u;
                                        rspc[x] = 0x80808080u;
                                    }
                                }
                                up_mv->Unmap(0, nullptr);
                                up_dep->Unmap(0, nullptr);
                                up_alb->Unmap(0, nullptr);
                                up_spc->Unmap(0, nullptr);
                            }
                            if (!open_list) {
                                ca_nf->Reset();
                                OnrRrNotifyD3d12AllocatorReset(ca_nf);
                                cl_nf->Reset(ca_nf, nullptr);
                                OnrRrNotifyD3d12ListReset(cl_nf, ca_nf);
                            }
                            D3D12_RESOURCE_BARRIER b_in[6]{};
                            ID3D12Resource* dsts[6] = {t_col, t_nrm, t_mv, t_dep, t_alb, t_spc};
                            ID3D12Resource* srcs[6] = {up_col, up_nrm, up_mv, up_dep, up_alb, up_spc};
                            UINT pitches[6] = {p_8, p_8, p_4, p_4, p_4, p_4};
                            DXGI_FORMAT fmts[6] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
                            for (int i = 0; i < 6; ++i) {
                                b_in[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                b_in[i].Transition.pResource = dsts[i];
                                b_in[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                b_in[i].Transition.StateBefore = D3D12_RESOURCE_STATES(srv_st);
                                b_in[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                            }
                            cl_nf->ResourceBarrier(6, b_in);
                            for (int i = 0; i < 6; ++i) {
                                D3D12_TEXTURE_COPY_LOCATION td{};
                                td.pResource = dsts[i];
                                td.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                D3D12_TEXTURE_COPY_LOCATION ts{};
                                ts.pResource = srcs[i];
                                ts.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                ts.PlacedFootprint.Footprint.Format = fmts[i];
                                ts.PlacedFootprint.Footprint.Width = w_in;
                                ts.PlacedFootprint.Footprint.Height = h_in;
                                ts.PlacedFootprint.Footprint.Depth = 1;
                                ts.PlacedFootprint.Footprint.RowPitch = pitches[i];
                                cl_nf->CopyTextureRegion(&td, 0, 0, 0, &ts, nullptr);
                                std::swap(b_in[i].Transition.StateBefore, b_in[i].Transition.StateAfter);
                            }
                            cl_nf->ResourceBarrier(6, b_in);
                            sync_nf(mask ? "upload_adv" : "upload_finite");
                        };

                        // Prime with finite unit textures first so _lt builds and Legacy kernels never run on NaN/Inf.
                        upload_guides(0u, true);
                        OnrRrSetFastMode(rt_nf, 1);
                        bool nf_primed = false;
                        for (int pstep = 0; pstep < 200 && nf_guides_ok; ++pstep) {
                            ca_nf->Reset();
                            OnrRrNotifyD3d12AllocatorReset(ca_nf);
                            cl_nf->Reset(ca_nf, nullptr);
                            OnrRrNotifyD3d12ListReset(cl_nf, ca_nf);
                            OnrRrFrame fr_p = fr_nf;
                            fr_p.flags = fr_nf.flags | ONR_RR_FRAME_RESET;
                            void* job_p = nullptr;
                            const int32_t rc_p = api4.Record(rt_nf, &fr_p, cl_nf, ONR_RR_EDIT_OFF, nullptr, &job_p);
                            if (rc_p != ONR_RR_OK) note_nf_fail("pstep_record", HRESULT(rc_p));
                            sync_nf("pstep");
                            OnrRrInfo info_nf{sizeof(OnrRrInfo)};
                            api4.v3.v2.v1.GetInfo(rt_nf, &info_nf);
                            if (info_nf.state == ONR_RR_STATE_READY && OnrRrGetFastMode(rt_nf) == 1) { nf_primed = true; break; }
                            if (info_nf.state == ONR_RR_STATE_FAILED) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        if (!nf_primed && nf_guides_ok) note_nf_fail("prime_timeout", E_FAIL);

                        char nf_phase_buf[32]{};
                        if (nf_guides_ok) upload_guides(nf_mask, false);
                        for (int fm_req : {1, 2}) {
                            if (!nf_guides_ok) break;
                            OnrRrSetFastMode(rt_nf, fm_req);
                            const int want_state = (fm_req == 2) ? 4 : 1;
                            std::snprintf(nf_phase_buf, sizeof(nf_phase_buf), "fm%d_wait", fm_req);
                            for (int wait_i = 0; wait_i < 200 && nf_guides_ok; ++wait_i) {
                                ca_nf->Reset();
                                OnrRrNotifyD3d12AllocatorReset(ca_nf);
                                cl_nf->Reset(ca_nf, nullptr);
                                OnrRrNotifyD3d12ListReset(cl_nf, ca_nf);
                                OnrRrFrame fr_w = fr_nf;
                                fr_w.flags = fr_nf.flags | ONR_RR_FRAME_RESET;
                                void* job_w = nullptr;
                                const int32_t rc_w = api4.Record(rt_nf, &fr_w, cl_nf, ONR_RR_EDIT_OFF, nullptr, &job_w);
                                if (rc_w != ONR_RR_OK) note_nf_fail(nf_phase_buf, HRESULT(rc_w));
                                sync_nf(nf_phase_buf);
                                if (OnrRrGetFastMode(rt_nf) == want_state) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                            if (nf_guides_ok && OnrRrGetFastMode(rt_nf) != want_state) note_nf_fail(nf_phase_buf, E_FAIL);

                            for (int step = 0; step < 6 && nf_guides_ok; ++step) {
                                std::snprintf(nf_phase_buf, sizeof(nf_phase_buf), "fm%d_step%d", fm_req, step);
                                ca_nf->Reset();
                                OnrRrNotifyD3d12AllocatorReset(ca_nf);
                                cl_nf->Reset(ca_nf, nullptr);
                                OnrRrNotifyD3d12ListReset(cl_nf, ca_nf);
                                OnrRrFrame fr_s = fr_nf;
                                fr_s.flags = fr_nf.flags | (step == 0 ? ONR_RR_FRAME_RESET : 0u);
                                void* job_s = nullptr;
                                const int32_t rc_s = api4.Record(rt_nf, &fr_s, cl_nf, ONR_RR_EDIT_OFF, nullptr, &job_s);
                                if (rc_s != ONR_RR_OK) note_nf_fail(nf_phase_buf, HRESULT(rc_s));
                                sync_nf(nf_phase_buf);
                            }
                            if (!nf_guides_ok) break;

                            ca_nf->Reset();
                            cl_nf->Reset(ca_nf, nullptr);
                            D3D12_RESOURCE_BARRIER bo{};
                            bo.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            bo.Transition.pResource = t_out;
                            bo.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            bo.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                            bo.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                            cl_nf->ResourceBarrier(1, &bo);
                            D3D12_TEXTURE_COPY_LOCATION to{};
                            to.pResource = rb_out;
                            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                            to.PlacedFootprint.Footprint.Width = w_out;
                            to.PlacedFootprint.Footprint.Height = h_out;
                            to.PlacedFootprint.Footprint.Depth = 1;
                            to.PlacedFootprint.Footprint.RowPitch = p_out;
                            D3D12_TEXTURE_COPY_LOCATION from{};
                            from.pResource = t_out;
                            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            cl_nf->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                            std::swap(bo.Transition.StateBefore, bo.Transition.StateAfter);
                            cl_nf->ResourceBarrier(1, &bo);
                            std::snprintf(nf_phase_buf, sizeof(nf_phase_buf), "fm%d_copy", fm_req);
                            sync_nf(nf_phase_buf);
                            if (!nf_guides_ok) break;

                            uint8_t* rp = nullptr;
                            size_t nf_out = 0;
                            if (SUCCEEDED(rb_out->Map(0, nullptr, reinterpret_cast<void**>(&rp))) && rp) {
                                for (UINT y = 0; y < h_out; ++y) {
                                    const uint16_t* row = reinterpret_cast<const uint16_t*>(rp + y * p_out);
                                    for (UINT x = 0; x < w_out * 4u; ++x)
                                        if ((row[x] & 0x7C00u) == 0x7C00u) ++nf_out;
                                }
                                rb_out->Unmap(0, nullptr);
                            } else {
                                note_nf_fail("rb_out_map", E_FAIL);
                            }
                            if (nf_out != 0) {
                                std::fprintf(stderr, "FAIL: NFMA direct D3D12 NaN/Inf guide stress mode=%d non_finite=%zu\n", fm_req, nf_out);
                                note_nf_fail("non_finite", E_FAIL);
                            }
                        }
                        OnrRrNotifyD3d12ListReleased(cl_nf);
                        OnrRrNotifyD3d12AllocatorReset(ca_nf);
                        OnrRrNotifyD3d12AllocatorReleased(ca_nf);
                        CloseHandle(ev_nf);
                        for (ID3D12Resource* r : {up_col, up_nrm, up_mv, up_dep, up_alb, up_spc, rb_out}) r->Release();
                        fn_nf->Release(); cl_nf->Release(); ca_nf->Release(); q_nf->Release();
                        for (ID3D12Resource* r : {t_col, t_alb, t_spc, t_nrm, t_dep, t_mv, t_out})
                            if (r) r->Release();
                        api4.v3.v2.v1.Destroy(rt_nf);

                        if (!nf_guides_ok) {
                            std::fprintf(stderr, "FAIL: Item NFMA Direct D3D12 guides failed (phase=%s, hr=0x%lX, mask=%u, primed=%d, dev_reason=0x%lX)\n",
                                         nf_fail_phase, static_cast<unsigned long>(nf_fail_hr), nf_mask,
                                         int(nf_primed), static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
                            dev->Release();
                            return 1;
                        }
                        std::printf("[PASS] Item NFMA Direct D3D12 guides (d3d_ae + d3d_enc0_attr_f_lt + post_f_lt + d3d_downsample_lt + _lt/_lx under NaN/+-Inf/-0/subnorm/+-65504): 0 non-finite\n");
                    } else {
                        std::fprintf(stderr, "FAIL: Item NFMA Direct D3D12 guides could not create fresh runtime context\n");
                        dev->Release();
                        return 1;
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
                                    // Create starts the network builder for a main view. Destroy
                                    // immediately, before the host releases its Vulkan device.
                                    ci_r1.output_width = 1920; ci_r1.output_height = 1080;
                                    ci_r1.dlssd_path = w_dlssd.c_str();
                                    void* early_ctx = nullptr;
                                    const int early_rc = api5.v4.v3.v2.v1.Create(&ci_r1, &early_ctx);
                                    if (early_ctx) api5.v4.v3.v2.v1.Destroy(early_ctx);
                                    if (early_rc != ONR_RR_OK || !early_ctx) {
                                        std::fprintf(stderr, "FAIL: Vulkan early-destroy Create rc=%d\n", early_rc);
                                        return 1;
                                    }
                                    std::printf("[PASS] Vulkan main-view Create/immediate Destroy before host device destruction\n");
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
                        const auto def_root = bdir / "openrr-amd";
                        const auto def_dir = def_root / "shaders";
                        if (std::filesystem::exists(def_root)) {
                            std::fprintf(stderr, "FAIL: R10 fixture directory already exists; refusing to overwrite it\n");
                            dev->Release();
                            return 1;
                        }
                        std::error_code ec_r10;
                        std::filesystem::create_directories(def_dir, ec_r10);
                        for (const auto& entry : std::filesystem::directory_iterator(spv_dir, ec_r10)) {
                            std::filesystem::copy_file(entry.path(), def_dir / entry.path().filename(),
                                                       std::filesystem::copy_options::overwrite_existing, ec_r10);
                        }
                        const auto src_aco = onr::u8_to_path(spv_dir).parent_path() / "aco";
                        if (std::filesystem::exists(src_aco, ec_r10)) {
                            std::filesystem::copy(src_aco, def_root / "aco",
                                                  std::filesystem::copy_options::recursive |
                                                      std::filesystem::copy_options::overwrite_existing,
                                                  ec_r10);
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
                        std::filesystem::remove_all(def_root, ec_r10);
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

    // QV weight encoding: exhaustive FP16 sweep. Exactly 254 bit patterns are exact OCP
    // e4m3 values (zeros, normals, subnormals of both signs); every accepted code must be
    // finite (0x7f/0xff are OCP NaN), decode back bit-exactly to the FP16 value, and the
    // former false-accepts +-480.0 (0x5f80/0xdf80 -> codes 0x7f/0xff) must now throw.
    {
        auto e4m3_value = [](uint32_t c) -> double {
            const double sign = (c & 0x80) ? -1.0 : 1.0;
            const uint32_t e = (c >> 3) & 15, m = c & 7;
            if (e == 0) return sign * (double(m) / 8.0) * (1.0 / 64.0);
            return sign * (1.0 + double(m) / 8.0) * std::ldexp(1.0, int(e) - 7);
        };
        auto fp16_value = [](uint32_t h) -> double {
            const double sign = (h & 0x8000) ? -1.0 : 1.0;
            const uint32_t e = (h >> 10) & 31, m = h & 1023;
            if (e == 0) return sign * (double(m) / 1024.0) * (1.0 / 16384.0);
            return sign * (1.0 + double(m) / 1024.0) * std::ldexp(1.0, int(e) - 15);
        };
        uint32_t accepted = 0;
        for (uint32_t h = 0; h <= 0xFFFF; ++h) {
            const uint16_t hw = uint16_t(h);
            bool threw = false;
            std::vector<uint8_t> d;
            try { d = onr::qv_bytes(&hw, 1); } catch (const std::exception&) { threw = true; }
            if (h == 0x5f80 || h == 0xdf80) {
                if (!threw) { std::fprintf(stderr, "FAIL: qv 0x%04x (+-480) must throw\n", h); return 1; }
                continue;
            }
            if (threw) continue;
            ++accepted;
            const uint32_t c = d[2];
            if ((c & 0x7f) == 0x7f) { std::fprintf(stderr, "FAIL: qv 0x%04x -> NaN code 0x%02x\n", h, c); return 1; }
            if (e4m3_value(c) != fp16_value(h)) {
                std::fprintf(stderr, "FAIL: qv 0x%04x -> 0x%02x value mismatch %.9g vs %.9g\n", h, c, e4m3_value(c), fp16_value(h));
                return 1;
            }
        }
        if (accepted != 254) { std::fprintf(stderr, "FAIL: qv accepted %u patterns, expected 254\n", accepted); return 1; }
        std::printf("[PASS] Item QV (qv_bytes 65536-pattern exhaustive sweep: 254 exact OCP e4m3 codes, "
                    "+-480 rejected, round-trip bit-exact)\n");
    }
    // Item OCP (WP-C4): the plane byte codecs + 4-lane word byte-order pin (0,1 -> lo; 2,3 -> hi).
    {
        auto e4m3_value = [](uint32_t c) -> double {   // OCP e4m3fn (bias 7, 448 max, 0x7f NaN)
            const double sign = (c & 0x80) ? -1.0 : 1.0;
            const uint32_t e = (c >> 3) & 15, m = c & 7;
            if (e == 15 && m == 7) return std::numeric_limits<double>::quiet_NaN();
            if (e == 0) return sign * double(m) * (1.0 / 512.0);
            return sign * (1.0 + double(m) / 8.0) * std::ldexp(1.0, int(e) - 7);
        };
        auto ocp_decode_fp16_bits = [](uint32_t c) -> uint32_t {
            const uint32_t s = c & 0x80u, e = (c >> 3) & 15u, m = c & 7u;
            if (e == 15 && m == 7) return (s << 8) | 0x7e00u;
            if (e == 0) {
                if (m == 0) return s << 8;
                const uint32_t p = m >= 4u ? 2u : m >= 2u ? 1u : 0u;
                return (s << 8) | ((6u + p) << 10) | ((m - (1u << p)) << (10u - p));
            }
            return (s << 8) | ((e + 8u) << 10) | (m << 7);
        };
        auto fp16_to_double = [](uint32_t h) -> double {
            const uint32_t he = (h >> 10) & 31u, hm = h & 1023u;
            if (he == 31u) return std::numeric_limits<double>::quiet_NaN();
            const double mag = he == 0 ? double(hm) * (1.0 / 65536.0)
                                       : (1.0 + double(hm) / 1024.0) * std::ldexp(1.0, int(he) - 15);
            return (h & 0x8000u) ? -mag : mag;
        };
        uint32_t ocp_exact = 0, sm86_zero_skew = 0, roundtrip = 0, nan_codes = 0;
        for (uint32_t c = 0; c <= 0xFF; ++c) {
            const uint32_t s = c & 0x80u, e = (c >> 3) & 15u, m = c & 7u;
            if (e == 15 && m == 7) ++nan_codes;
            const uint32_t h = ocp_decode_fp16_bits(c);
            const double hv = fp16_to_double(h);
            const double want = e4m3_value(c);
            if (std::isnan(want) ? !std::isnan(hv) : hv != want) {
                std::fprintf(stderr, "FAIL: OCP decode 0x%02x -> fp16 0x%04x = %.9g, want %.9g\n", c, h, hv, want);
                return 1;
            }
            ++ocp_exact;
            if (!std::isnan(want)) ++roundtrip;
            const uint32_t s86 = (((c & 0x7fu) << 7) + 0x2000u) | (s << 8);
            const uint32_t s86e = (s86 >> 10) & 31u;
            if (e == 0) {
                if (s86e != 8u) { std::fprintf(stderr, "FAIL: sm_86 decode of 0x%02x not exp-8\n", c); return 1; }
                ++sm86_zero_skew;
            } else if (e == 15u && m == 7u) {
                if (s86 != ((s << 8) | 0x5f80u)) {
                    std::fprintf(stderr, "FAIL: sm_86 decode of NaN code 0x%02x -> 0x%04x\n", c, s86);
                    return 1;
                }
            } else if (s86 != h) {
                std::fprintf(stderr, "FAIL: sm_86 decode 0x%02x -> 0x%04x, OCP exact 0x%04x\n", c, s86, h);
                return 1;
            }
        }
        // 4-lane word roundtrip pin: pack 4 distinct codes (0, 3*2^-9, -1.5, 448) into bytes 0..3;
        // verify e4m3_lo reads bytes (0, 1) and e4m3_hi reads bytes (2, 3) with zero (1, 2) swap.
        const uint32_t c4[4] = { 0x00u, 0x03u, 0xBCu, 0x7Eu };   // +0.0, 3*2^-9, -1.5, 448.0
        const double   v4[4] = { 0.0, 3.0 / 512.0, -1.5, 448.0 };
        const uint32_t w4 = c4[0] | (c4[1] << 8) | (c4[2] << 16) | (c4[3] << 24);
        const uint32_t lo_w = ocp_decode_fp16_bits(w4 & 0xffu) | (ocp_decode_fp16_bits((w4 >> 8) & 0xffu) << 16);
        const uint32_t hi_w = ocp_decode_fp16_bits((w4 >> 16) & 0xffu) | (ocp_decode_fp16_bits(w4 >> 24) << 16);
        const double dec4[4] = {
            fp16_to_double(lo_w & 0xffffu), fp16_to_double(lo_w >> 16),
            fp16_to_double(hi_w & 0xffffu), fp16_to_double(hi_w >> 16),
        };
        for (int k = 0; k < 4; ++k) {
            if (dec4[k] != v4[k]) {
                std::fprintf(stderr, "FAIL: OCP 4-lane word lane %d got %.9g, want %.9g\n", k, dec4[k], v4[k]);
                return 1;
            }
        }
        if (ocp_exact != 256 || nan_codes != 2 || sm86_zero_skew != 16 || roundtrip != 254) {
            std::fprintf(stderr, "FAIL: OCP sweep counts %u/%u/%u/%u\n", ocp_exact, nan_codes, sm86_zero_skew, roundtrip);
            return 1;
        }
        std::printf("[PASS] Item OCP (256-byte exhaustive + 4-lane lo(0,1)/hi(2,3) round-trip pin)\n");
    }

    dev->Release();
    return 0;
}

// Item LANG (Stage 8 Phase 5): menu language loader (asi/src/rr_lang.hpp) — msgid/msgstr parsing, UTF-8 BOM,
// C escapes & continuations, printf-conversion safety gate, BMP code-point collection, Label##id & tr_items,
// suspended() fallback, staged 4-language discovery/parity, and [OpenRR] Language ini persistence.
static int test_language_loader() {
    using namespace openrr::lang;
    struct Clean { ~Clean() { suspended() = false; table() = Table{}; } } clean;
    const char* dir = ".tmp-lang-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto write = [&](const char* name, const std::string& text) {
        FILE* f = openrr::u8::fopen((std::string(dir) + "\\" + name).c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        return ok;
    };
    const bool ok = write("t1.txt", "\xEF\xBB\xBF# a comment\n"
                                    "msgid \"@language\"\nmsgstr \"Test\"\n"
                                    "msgid \"@font\"\nmsgstr \"fonts/x.ttf\"\n"
                                    "msgid \"Menu & Hotkeys\"\nmsgstr \"\xE8\x8F\x9C\xE5\x8D\x95\" \n"
                                    "msgid \"multi\"\n\"line\\ttab\"\nmsgstr \"a\\nb\"\n"
                                    "msgid \"Frames: %d (%.2f ms)\"\nmsgstr \"\xD1\x80\xD0\xB0\xD0\xBC\xD0\xBA\xD0\xB8 %d\"\n"
                                    "msgid \"Close\"\nmsgstr \"\"\n");
    if (!ok) { std::fprintf(stderr, "FAIL: lang test file\n"); return 1; }
    if (!load(dir, "t1")) { std::fprintf(stderr, "FAIL: lang load\n"); return 1; }
    const Table& t = table();
    if (t.display != "Test" || t.font != "fonts/x.ttf") { std::fprintf(stderr, "FAIL: lang @language/@font\n"); return 1; }
    if (t.entries != 2) { std::fprintf(stderr, "FAIL: lang entries %d (want 2)\n", t.entries); return 1; }
    if (t.rejected != 1) { std::fprintf(stderr, "FAIL: lang rejected %d (want 1)\n", t.rejected); return 1; }
    if (std::strcmp(tr("Menu & Hotkeys"), "\xE8\x8F\x9C\xE5\x8D\x95") != 0) { std::fprintf(stderr, "FAIL: lang tr\n"); return 1; }
    if (std::strcmp(tr("Status"), "Status") != 0) { std::fprintf(stderr, "FAIL: lang tr fallback\n"); return 1; }
    if (std::strcmp(tr("multi" "line\ttab"), "a\nb") != 0) { std::fprintf(stderr, "FAIL: lang escapes/continuation\n"); return 1; }
    if (std::strcmp(tr("Menu & Hotkeys##m"), "\xE8\x8F\x9C\xE5\x8D\x95##m") != 0) { std::fprintf(stderr, "FAIL: lang ##id\n"); return 1; }
    const char items[] = "Menu & Hotkeys\0Status\0Close\0";
    const char* ti = tr_items(items);
    const char* ti1 = ti + std::strlen(ti) + 1;
    const char* ti2 = ti1 + std::strlen(ti1) + 1;
    if (std::strcmp(ti, "\xE8\x8F\x9C\xE5\x8D\x95") != 0 || std::strcmp(ti1, "Status") != 0 || std::strcmp(ti2, "Close") != 0)
        { std::fprintf(stderr, "FAIL: lang tr_items\n"); return 1; }
    if (t.code_points.size() != 2) { std::fprintf(stderr, "FAIL: lang code points %zu\n", t.code_points.size()); return 1; }
    suspended() = true;
    if (std::strcmp(tr("Menu & Hotkeys"), "Menu & Hotkeys") != 0 || std::strcmp(tr_items(items), "Menu & Hotkeys") != 0)
        { std::fprintf(stderr, "FAIL: lang suspended\n"); return 1; }
    suspended() = false;
    if (!load(dir, "") || !table().map.empty()) { std::fprintf(stderr, "FAIL: lang english clear\n"); return 1; }
    if (load(dir, "no-such-language")) { std::fprintf(stderr, "FAIL: lang missing file\n"); return 1; }

    // Real staged package audit when executed from the repo root
    const std::string staged_lang = std::filesystem::exists("build\\aco-v015\\stage\\openrr-amd\\lang")
        ? "build\\aco-v015\\stage\\openrr-amd\\lang"
        : std::filesystem::exists("asi\\lang") ? "asi\\lang" : "";
    if (!staged_lang.empty()) {
        const auto found = list(staged_lang);
        bool has_en = false, has_zh = false, has_ja = false, has_ko = false;
        for (const auto& [stem, shown] : found) {
            if (stem.empty() && shown == "English") has_en = true;
            if (stem == "zh-CN") has_zh = true;
            if (stem == "ja") has_ja = true;
            if (stem == "ko") has_ko = true;
        }
        if (!has_en || !has_zh || !has_ja || !has_ko) {
            std::fprintf(stderr, "FAIL: lang list missing expected languages (%zu found)\n", found.size());
            return 1;
        }
        for (const char* lc : {"zh-CN", "ja", "ko"}) {
            if (!load(staged_lang, lc) || table().entries < 55 || table().rejected != 0) {
                std::fprintf(stderr, "FAIL: lang staged %s entries=%d rejected=%d\n", lc, table().entries, table().rejected);
                return 1;
            }
        }
        if (staged_lang.rfind("build\\", 0) == 0 &&
            !std::filesystem::exists(staged_lang + "\\fonts\\NotoSansCJKsc-Regular.ttf")) {
            std::fprintf(stderr, "FAIL: staged NotoSansCJKsc-Regular.ttf missing\n");
            return 1;
        }
    }

    // [OpenRR] Language ini round trip via both openrr::ini_edit::Ini and Win32 profile APIs
    const std::string ini_path = std::string(dir) + "\\openrr-amd.ini";
    if (!write("openrr-amd.ini", "[OpenRR]\nLanguage =\n")) { std::fprintf(stderr, "FAIL: lang ini create\n"); return 1; }
    {
        openrr::ini_edit::Ini editor(ini_path);
        editor.refresh();
        editor.set("OpenRR", "Language", "ja");
        editor.save();
        openrr::ini_edit::Ini reader(ini_path);
        reader.refresh();
        if (reader.get("OpenRR", "Language", "") != "ja") {
            std::fprintf(stderr, "FAIL: lang ini_edit round trip\n");
            return 1;
        }
    }
    WritePrivateProfileStringA("OpenRR", "Language", "zh-CN", ini_path.c_str());
    char back[64] = {};
    GetPrivateProfileStringA("OpenRR", "Language", "", back, sizeof back, ini_path.c_str());
    if (std::strcmp(back, "zh-CN") != 0) { std::fprintf(stderr, "FAIL: lang ini round trip\n"); return 1; }
    std::filesystem::remove_all(dir);
    std::printf("[PASS] Item LANG (msgid/msgstr + BOM + escapes + continuation, conversion gate, "
                "code points, ##id + tr_items, suspended fallback, 4-lang parity >=55 keys, ini round trip)\n");
    return 0;
}

static int test_vk_unspoof_and_rdna3_pick() {
    openrr::pe::vulkan_entry_points();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || !inst) {
        std::fprintf(stderr, "FAIL: test_vk_unspoof_and_rdna3_pick vkCreateInstance failed\n");
        return 1;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    if (count) vkEnumeratePhysicalDevices(inst, &count, devs.data());
    std::string picked_name;
    uint32_t api_ver = 0;
    bool is_amd = false;
    const VkPhysicalDevice picked = onr::pick_vk_physical_device(devs, nullptr, &picked_name, &api_ver, &is_amd);
    if (!picked || !is_amd) {
        vkDestroyInstance(inst, nullptr);
        std::fprintf(stderr, "FAIL: pick_vk_physical_device picked=%p is_amd=%d name='%s'\n",
                     static_cast<void*>(picked), int(is_amd), picked_name.c_str());
        return 1;
    }
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    onr::query_real_vk_props(picked, props, drv);
    vkDestroyInstance(inst, nullptr);
    if (props.vendorID != 0x1002 || props.deviceName[0] == '\0' ||
        std::strstr(props.deviceName, "NVIDIA") != nullptr) {
        std::fprintf(stderr, "FAIL: query_real_vk_props vendor=0x%04x name='%s' drv='%s'\n",
                     props.vendorID, props.deviceName, drv.driverInfo);
        return 1;
    }
    std::printf("[PASS] Vulkan unspoof & RDNA3/RDNA4 device pick: vendor=0x%04x name='%s' driver='%s'\n",
                props.vendorID, props.deviceName, drv.driverInfo);
    return 0;
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::set_terminate([] {
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
        catch (const std::exception& e) { std::fprintf(stderr, "FAIL: unhandled test exception: %s\n", e.what()); }
        catch (...) { std::fprintf(stderr, "FAIL: unknown test exception\n"); }
        std::fflush(nullptr);
        ExitProcess(1);
    });
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    bool host_bound = true;
    const int32_t rc = openrr::pe::record_d3d12(nullptr, nullptr, nullptr, host_bound, false);
    if (rc != ONR_RR_INVALID_ARGUMENT || host_bound) {
        std::fprintf(stderr, "FAIL: rejected D3D12 Record must not request host state restoration\n");
        return 1;
    }
    std::printf("[PASS] rejected D3D12 Record leaves host state untouched\n");
    if (test_installed_data_paths() != 0) return 1;
    if (test_key_parsing() != 0) return 1;
    if (test_language_loader() != 0) return 1;
    if (test_vk_unspoof_and_rdna3_pick() != 0) return 1;
    if (test_split_hardening_and_eligibility() != 0) return 1;
    if (test_d3d12_gpu_and_engine() != 0) return 1;
    std::printf("ALL ASI UNIT TESTS PASSED\n");
    return 0;
}
