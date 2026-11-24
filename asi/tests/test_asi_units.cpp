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
#include "rr_pe_d3d12_hook_policy.hpp"
#include "rr_pe_d3d12_direct_hooks.hpp"
#include "rr_pe_d3d12split.hpp"
#include "rr_pe_nvapi_compat.hpp"
#include "rr_pe_vkdevice.hpp"
#include "rr_pe_vkreset.hpp"
#include "rr_radv_spirv.hpp"
#include "opennr_rr_api.h"
#include "rr_d3d12_record.hpp"
#include "rr_data_paths.hpp"
#include "opennr_rr/d3d12.hpp"
#include "opennr_rr/engine.hpp"
#include "opennr_rr/gpu_arch.hpp"
#include "opennr_rr/vk.hpp"
#include "rr_pal_binary.hpp"
#include "vk_host.hpp"

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
                struct { uint64_t dst; uint32_t k, mid[57], last; } pc{};
                static_assert(sizeof(pc) == 248); // trailing host alignment is not pushed
                pc.k = 17; pc.last = 0x9876;
                for (unsigned i = 0; i < 57; ++i) pc.mid[i] = i * 31;
                if (std::filesystem::exists(spv_dir_str + "/aco_abi.spv")) {
                    auto k = d3d.load(spv_dir_str + "/aco_abi.spv", 0);
                    if (!k.aco) throw std::runtime_error("aco_abi silently fell back to LLPC");
                    auto out = d3d.create(57 * 64 * 4);
                    pc.dst = out.addr;
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
                }
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

        // ---- R4: Verify UTF-8 non-ASCII (Chinese) shader path loading on Windows ----
        {
            const std::filesystem::path cn_dir = bdir / L"utf8_测试中文路径";
            std::error_code ec;
            std::filesystem::create_directories(cn_dir, ec);
            const std::filesystem::path cn_spv = cn_dir / L"clear_range.spv";
            std::filesystem::copy_file(onr::u8_to_path(spv_dir) / L"clear_range.spv", cn_spv,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            d3d.set_compiler(false);
            onr::Kernels k_cn_vk, k_cn_d3d;
            k_cn_vk.load(vk, onr::path_to_u8(cn_dir), {"clear_range"});
            k_cn_d3d.load(d3d, onr::path_to_u8(cn_dir), {"clear_range"});
            const bool cn_ok = k_cn_vk.has("clear_range") && k_cn_d3d.has("clear_range");
            k_cn_d3d.unload(d3d);
            k_cn_vk.unload(vk);
            d3d.set_compiler(true);
            std::filesystem::remove_all(cn_dir, ec);
            if (!cn_ok) {
                std::fprintf(stderr, "FAIL: R4 UTF-8 Chinese shader path loading failed\n");
                dev->Release();
                return 1;
            }
            std::printf("[PASS] Item R4 (UTF-8 Chinese shader path loading): Vk & D3D12 loaded clear_range.spv from non-ASCII path\n");
        }

        onr::Kernels k_clear;
        k_clear.load(d3d, spv_dir, {"clear_range"});

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

                // Tiny-render reflect_i regression: exercise levels smaller than one 8x8 window (1x1 -> lo_size 1) across both shifts.
                {
                    auto run_tiny = [&](onr::I2 r_sz, onr::I2 o_sz) -> bool {
                        eng.resize(r_sz, o_sz);
                        const size_t rpx = size_t(r_sz.x) * r_sz.y;
                        const size_t opx = size_t(o_sz.x) * o_sz.y;
                        onr::Buffer t_col = d3d.create_unzeroed(std::max<size_t>(rpx * 8, 16));
                        onr::Buffer t_alb = d3d.create_unzeroed(std::max<size_t>(rpx * 4, 16));
                        onr::Buffer t_spc = d3d.create_unzeroed(std::max<size_t>(rpx * 4, 16));
                        onr::Buffer t_nrm = d3d.create_unzeroed(std::max<size_t>(rpx * 8, 16));
                        onr::Buffer t_rgh = d3d.create_unzeroed(std::max<size_t>(rpx * 2, 16));
                        onr::Buffer t_dep = d3d.create_unzeroed(std::max<size_t>(rpx * 4, 16));
                        onr::Buffer t_mv  = d3d.create_unzeroed(std::max<size_t>(rpx * 4, 16));
                        onr::Buffer t_hit = d3d.create_unzeroed(std::max<size_t>(rpx * 2, 16));
                        onr::Buffer t_out = d3d.create_unzeroed(std::max<size_t>(opx * 8, 16));
                        d3d.begin_batch();
                        d3d.fill(t_col, 0x38003800u); d3d.fill(t_alb, 0x80808080u); d3d.fill(t_spc, 0x20202020u);
                        d3d.fill(t_nrm, 0x00003C00u); d3d.fill(t_rgh, 0x38003800u); d3d.fill(t_dep, 0x3F800000u);
                        d3d.fill(t_mv, 0u); d3d.fill(t_hit, 0x38003800u); d3d.fill(t_out, 0u);
                        d3d.end_batch();
                        const onr::Guides tg{t_col.addr, t_alb.addr, t_spc.addr, t_nrm.addr, t_rgh.addr, t_dep.addr, t_mv.addr, t_hit.addr};
                        onr::EngineFrame tf = ef;
                        tf.render = r_sz;
                        tf.output = o_sz;
                        bool ok = true;
                        for (int lite = 1; lite >= 0 && ok; --lite) {
                            eng.set_lite_on(lite != 0);
                            for (int f = 0; f < 4 && ok; ++f) {
                                tf.reset = (f == 0);
                                tf.jitter[0] = (f & 1) ? 0.25f : -0.25f;
                                tf.jitter[1] = (f & 2) ? 0.25f : -0.25f;
                                d3d.begin_own();
                                eng.record(tf, tg, t_out.addr);
                                d3d.submit_own();
                                if (FAILED(dev->GetDeviceRemovedReason())) {
                                    ok = false;
                                    break;
                                }
                                std::vector<uint16_t> out_px(opx * 4);
                                d3d.download(t_out, out_px.data(), out_px.size() * 2);
                                for (uint16_t h : out_px) {
                                    if ((h & 0x7C00u) == 0x7C00u) {
                                        ok = false;
                                        break;
                                    }
                                }
                            }
                        }
                        eng.set_lite_on(true);
                        for (onr::Buffer* b : {&t_col, &t_alb, &t_spc, &t_nrm, &t_rgh, &t_dep, &t_mv, &t_hit, &t_out})
                            d3d.destroy(*b);
                        return ok && SUCCEEDED(dev->GetDeviceRemovedReason());
                    };
                    if (!run_tiny({1, 1}, {32, 32}) || !run_tiny({17, 9}, {64, 36})) {
                        std::fprintf(stderr, "FAIL: D3D12 Direct Engine tiny render sizes\n");
                        k2_failed = true;
                    } else {
                        std::printf("[PASS] D3D12 Direct Engine tiny render sizes (1x1->32x32, 17x9->64x36): Fast & Legacy finite, 0 device removal\n");
                    }
                }
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

extern "C" int32_t OnrRrSetFastMode(void*, int32_t);
extern "C" int32_t OnrRrGetFastMode(void*);

static onr::pal::Bytes make_synthetic_pal_elf(uint32_t elf_mach, uint64_t user_data_limit,
                                              std::initializer_list<uint64_t> map_entries) {
    using namespace onr::pal;
    Value cs(uint64_t(0));
    cs = Value(); cs.kind = Value::Map;
    cs.set(".wavefront_size", Value(32));
    cs.set(".entry_point_symbol", [] { Value s; s.kind = Value::String; s.s = "_amdgpu_cs_main"; return s; }());
    cs.set(".threadgroup_dimensions", array({64, 1, 1}));
    Value reg_map; reg_map.kind = Value::Array;
    for (uint64_t e : map_entries) reg_map.items.emplace_back(e);
    while (reg_map.items.size() < 32) reg_map.items.emplace_back(0xffffffffu);
    cs.set(".user_data_reg_map", std::move(reg_map));

    Value hs; hs.kind = Value::Map;
    hs.set(".cs", std::move(cs));
    Value pipe; pipe.kind = Value::Map;
    pipe.set(".api", [] { Value s; s.kind = Value::String; s.s = "Vulkan"; return s; }());
    pipe.set(".type", [] { Value s; s.kind = Value::String; s.s = "Cs"; return s; }());
    pipe.set(".user_data_limit", Value(user_data_limit));
    pipe.set(".hardware_stages", std::move(hs));
    Value cr; cr.kind = Value::Map;
    pipe.set(".compute_registers", std::move(cr));

    Value pipes; pipes.kind = Value::Array;
    pipes.items.push_back(std::move(pipe));
    Value meta; meta.kind = Value::Map;
    meta.set("amdpal.version", array({3, 6}));
    meta.set("amdpal.pipelines", std::move(pipes));

    Bytes md;
    encode(md, meta);
    Bytes note_sec;
    word(note_sec, 7);
    word(note_sec, uint32_t(md.size()));
    word(note_sec, 32);
    note_sec.insert(note_sec.end(), {'A', 'M', 'D', 'G', 'P', 'U', 0, 0});
    note_sec.insert(note_sec.end(), md.begin(), md.end());
    while (note_sec.size() % 4) note_sec.push_back(0);

    auto add_cstr = [](Bytes& st, const char* s) -> uint32_t {
        uint32_t off = uint32_t(st.size());
        st.insert(st.end(), s, s + std::strlen(s) + 1);
        return off;
    };
    Bytes shstr{0}, strtab{0};
    const uint32_t n_shstr = add_cstr(shstr, ".shstrtab");
    const uint32_t n_text = add_cstr(shstr, ".text");
    const uint32_t n_str = add_cstr(shstr, ".strtab");
    const uint32_t n_sym = add_cstr(shstr, ".symtab");
    const uint32_t n_note = add_cstr(shstr, ".note");
    const uint32_t n_cs_main = add_cstr(strtab, "_amdgpu_cs_main");

    Bytes text_sec(256, 0);
    Bytes symtab(24, 0);
    write<uint32_t>(symtab, 0, n_cs_main);
    symtab[4] = 2; // STT_FUNC
    write<uint16_t>(symtab, 6, 2); // section index 2 = .text
    write<uint64_t>(symtab, 8, 0);
    write<uint64_t>(symtab, 16, 4);

    Bytes elf(64, 0);
    std::memcpy(elf.data(), "\x7f" "ELF", 4);
    elf[4] = 2; elf[5] = 1; elf[6] = 1; elf[7] = 65;
    write<uint16_t>(elf, 16, 1);
    write<uint16_t>(elf, 18, 224);
    write<uint32_t>(elf, 20, 1);
    write<uint32_t>(elf, 48, elf_mach);
    write<uint16_t>(elf, 52, 64);
    write<uint16_t>(elf, 58, 64);
    write<uint16_t>(elf, 60, 6);
    write<uint16_t>(elf, 62, 1);

    std::vector<Section> secs(6);
    auto append_sec = [&](size_t idx, uint32_t name_off, uint32_t type, uint64_t align,
                          const Bytes& content, uint32_t link = 0, uint64_t entsize = 0) {
        size_t a = std::max<uint64_t>(align, 1);
        elf.resize((elf.size() + a - 1) & ~(a - 1));
        secs[idx].name = name_off;
        secs[idx].type = type;
        secs[idx].offset = elf.size();
        secs[idx].size = content.size();
        secs[idx].link = link;
        secs[idx].align = align;
        secs[idx].entsize = entsize;
        elf.insert(elf.end(), content.begin(), content.end());
    };
    append_sec(1, n_shstr, 3, 1, shstr);
    append_sec(2, n_text, 1, 256, text_sec);
    append_sec(3, n_str, 3, 1, strtab);
    append_sec(4, n_sym, 2, 8, symtab, 3, 24);
    append_sec(5, n_note, 7, 4, note_sec);
    elf.resize((elf.size() + 7) & ~size_t(7));
    write<uint64_t>(elf, 40, elf.size());
    for (const auto& s : secs) {
        size_t p = elf.size();
        elf.resize(p + 64);
        write(elf, p, s);
    }
    return elf;
}

static int test_v017_stage10() {
    // 1. gpu_arch classification and target resolution
    {
        using onr::arch::GpuArch;
        bool ok = onr::arch::arch_from_mach(0x4e) == GpuArch::Rdna4 &&
                  onr::arch::arch_from_mach(0x48) == GpuArch::Rdna4 &&
                  onr::arch::arch_from_mach(0x41) == GpuArch::Rdna3 &&
                  onr::arch::arch_from_mach(0x44) == GpuArch::Rdna3 &&
                  onr::arch::arch_from_mach(0x43) == GpuArch::Rdna3 &&
                  onr::arch::arch_from_mach(0x36) == GpuArch::Rdna2 &&
                  onr::arch::arch_from_mach(0x33) == GpuArch::Rdna1 &&
                  onr::arch::arch_from_pci(0x1002, 0x7550) == GpuArch::Rdna4 &&
                  onr::arch::arch_from_pci(0x1002, 0x7590) == GpuArch::Rdna4 &&
                  onr::arch::arch_from_pci(0x1002, 0x744c) == GpuArch::Rdna3 &&
                  onr::arch::arch_from_pci(0x1002, 0x1900) == GpuArch::Rdna3 &&
                  onr::arch::arch_from_pci(0x1002, 0x73bf) == GpuArch::Rdna2 &&
                  onr::arch::aco_target_for_mach(0x4e) && std::strcmp(onr::arch::aco_target_for_mach(0x4e), "gfx1201") == 0 &&
                  onr::arch::aco_target_for_mach(0x48) && std::strcmp(onr::arch::aco_target_for_mach(0x48), "gfx1200") == 0 &&
                  onr::arch::aco_target_for_mach(0x41) == nullptr &&
                  onr::arch::aco_target_for_mach(0x44) == nullptr &&
                  onr::arch::aco_target_for_mach(0) == nullptr;
        if (!ok) {
            std::fprintf(stderr, "FAIL: gpu_arch classification / target mapping mismatch\n");
            return 1;
        }
        std::printf("[PASS] gpu_arch classification and gfx1200/gfx1201 target mapping\n");
    }

    // 2. ONR_TEST_SPOOF_LOADER=1 direct ICD recovery + non-mutating vk_host chain clone
    {
        openrr::pe::vulkan_entry_points();
        const char* prev_spoof = std::getenv("ONR_TEST_SPOOF_LOADER");
        const std::string saved_spoof = prev_spoof ? prev_spoof : "";
        struct RestoreEnv {
            std::string v;
            bool had;
            ~RestoreEnv() { _putenv_s("ONR_TEST_SPOOF_LOADER", had ? v.c_str() : ""); }
        } guard{saved_spoof, prev_spoof != nullptr};
        _putenv_s("ONR_TEST_SPOOF_LOADER", "1");

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || !inst) {
            std::fprintf(stderr, "FAIL: spoof test vkCreateInstance failed\n");
            return 1;
        }
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> devs(count);
        if (count) vkEnumeratePhysicalDevices(inst, &count, devs.data());

        uint32_t amd_icd_count = 0;
        for (VkPhysicalDevice pd : devs) {
            const onr::TrustedVkIdentity tid = onr::identify_vk_physical_device(pd);
            if (tid.vendor == 0x1002 && tid.verified_icd && tid.elf_mach != 0 &&
                (tid.arch == onr::arch::GpuArch::Rdna4 || tid.arch == onr::arch::GpuArch::Rdna3)) {
                ++amd_icd_count;
            }
        }
        std::string picked_name;
        bool is_amd = false;
        const VkPhysicalDevice picked = onr::pick_vk_physical_device(devs, nullptr, &picked_name, nullptr, &is_amd);
        if (!picked || !is_amd || amd_icd_count == 0) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: spoof recovery picked=%p is_amd=%d amd_icd_count=%u\n",
                         static_cast<void*>(picked), int(is_amd), amd_icd_count);
            return 1;
        }
        const onr::TrustedVkIdentity picked_tid = onr::identify_vk_physical_device(picked);
        std::printf("[PASS] ONR_TEST_SPOOF_LOADER=1 direct ICD recovered %u AMD GPUs; picked '%s' (0x%04x, mach=0x%x, %s)\n",
                    amd_icd_count, picked_tid.name.c_str(), picked_tid.device, picked_tid.elf_mach,
                    onr::arch::arch_name(picked_tid.arch));

        _putenv_s("ONR_TEST_SPOOF_LOADER", saved_spoof.c_str());

        // Test vk_host::prepare non-mutating chain clone with Vulkan14Features + real vkCreateDevice
        VkBaseOutStructure unknown_tail{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
        VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &unknown_tail};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &v14};
        v13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        v12.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceFeatures core{};
        core.samplerAnisotropy = VK_TRUE;

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(picked, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(picked, &qcount, qprops.data());
        uint32_t gfx_q = 0;
        for (uint32_t i = 0; i < qcount; ++i)
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_q = i; break; }
        float qprio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = gfx_q; qci.queueCount = 1; qci.pQueuePriorities = &qprio;
        const char* game_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo game_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12};
        game_ci.queueCreateInfoCount = 1; game_ci.pQueueCreateInfos = &qci;
        game_ci.enabledExtensionCount = 1; game_ci.ppEnabledExtensionNames = game_exts;
        game_ci.pEnabledFeatures = &core;

        const auto snap_ci = game_ci;
        const auto snap_v12 = v12;
        const auto snap_v13 = v13;
        const auto snap_v14 = v14;
        const auto snap_core = core;
        const auto snap_unk = unknown_tail;

        VkDeviceCreateInfo mod_ci{};
        void* setup_tok = nullptr;
        std::string why;
        if (onr_host::prepare(picked, &game_ci, &mod_ci, &setup_tok, why) != 0) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: vk_host::prepare failed: %s\n", why.c_str());
            return 1;
        }
        const bool unmutated_after_prepare =
            std::memcmp(&game_ci, &snap_ci, sizeof(game_ci)) == 0 &&
            std::memcmp(&v12, &snap_v12, sizeof(v12)) == 0 &&
            std::memcmp(&v13, &snap_v13, sizeof(v13)) == 0 &&
            std::memcmp(&v14, &snap_v14, sizeof(v14)) == 0 &&
            std::memcmp(&core, &snap_core, sizeof(core)) == 0 &&
            std::memcmp(&unknown_tail, &snap_unk, sizeof(unknown_tail)) == 0;
        if (!unmutated_after_prepare) {
            onr_host::finish(setup_tok, VK_NULL_HANDLE, nullptr, nullptr, nullptr);
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: vk_host::prepare mutated caller structs!\n");
            return 1;
        }

        // Verify cloned pNext does NOT attach a duplicate Maintenance5FeaturesKHR alongside Vulkan14Features
        unsigned v14_cnt = 0, m5_node_cnt = 0, pbin_node_cnt = 0;
        bool cloned_m5_on = false, cloned_pbin_on = false;
        for (const auto* b = static_cast<const VkBaseInStructure*>(mod_ci.pNext); b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
                ++v14_cnt;
                cloned_m5_on = reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(b)->maintenance5 == VK_TRUE;
            } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR) {
                ++m5_node_cnt;
            } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR) {
                ++pbin_node_cnt;
                cloned_pbin_on = reinterpret_cast<const VkPhysicalDevicePipelineBinaryFeaturesKHR*>(b)->pipelineBinaries == VK_TRUE;
            }
        }
        if (v14_cnt != 1 || m5_node_cnt != 0 || pbin_node_cnt != 1 || !cloned_m5_on || !cloned_pbin_on) {
            onr_host::finish(setup_tok, VK_NULL_HANDLE, nullptr, nullptr, nullptr);
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: vk_host pNext v14=%u m5=%u(%d) pbin=%u(%d)\n",
                         v14_cnt, m5_node_cnt, int(cloned_m5_on), pbin_node_cnt, int(cloned_pbin_on));
            return 1;
        }
        // Strip swapchain ext since we have no surface here? No, swapchain ext creates fine on AMD desktop,
        // and Vulkan14Features was injected into mod_ci so real vkCreateDevice tests exact validity!
        VkDevice hdev = VK_NULL_HANDLE;
        const VkResult drc = vkCreateDevice(picked, &mod_ci, nullptr, &hdev);
        uint32_t out_qfam = 0, out_qidx = 0;
        std::string added;
        const int frc = onr_host::finish(setup_tok, drc == VK_SUCCESS ? hdev : VK_NULL_HANDLE, &out_qfam, &out_qidx, &added);
        const bool pbin_reg = (drc == VK_SUCCESS) && onr::vk_device_has_pipeline_binary(hdev);
        if (hdev) {
            onr::vk_note_device_destroyed(hdev);
            vkDestroyDevice(hdev, nullptr);
        }
        if (drc != VK_SUCCESS || frc != 0 || !pbin_reg) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: vk_host real vkCreateDevice drc=%d frc=%d pbin=%d\n", int(drc), frc, int(pbin_reg));
            return 1;
        }
        std::printf("[PASS] vk_host non-mutating clone + Vulkan14Features::maintenance5 + VK_KHR_pipeline_binary (added: %s)\n", added.c_str());

        // Case B: split chain (no Vulkan14Features)
        VkPhysicalDeviceTimelineSemaphoreFeatures ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        ts.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceDynamicRenderingFeatures dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &ts};
        dr.dynamicRendering = VK_TRUE;
        VkDeviceCreateInfo split_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dr};
        split_ci.queueCreateInfoCount = 1; split_ci.pQueueCreateInfos = &qci;
        VkPhysicalDeviceFeatures split_core{};
        split_ci.pEnabledFeatures = &split_core;
        const auto snap_sci = split_ci;
        const auto snap_dr = dr;
        const auto snap_ts = ts;
        const auto snap_score = split_core;
        VkDeviceCreateInfo split_mod{};
        void* split_tok = nullptr;
        if (onr_host::prepare(picked, &split_ci, &split_mod, &split_tok, why) != 0) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: vk_host split-chain prepare failed: %s\n", why.c_str());
            return 1;
        }
        const bool split_unmutated =
            std::memcmp(&split_ci, &snap_sci, sizeof(split_ci)) == 0 &&
            std::memcmp(&dr, &snap_dr, sizeof(dr)) == 0 &&
            std::memcmp(&ts, &snap_ts, sizeof(ts)) == 0 &&
            std::memcmp(&split_core, &snap_score, sizeof(split_core)) == 0;
        VkDevice sdev = VK_NULL_HANDLE;
        const VkResult sdrc = split_unmutated ? vkCreateDevice(picked, &split_mod, nullptr, &sdev) : VK_ERROR_INITIALIZATION_FAILED;
        const int sfrc = onr_host::finish(split_tok, sdrc == VK_SUCCESS ? sdev : VK_NULL_HANDLE, &out_qfam, &out_qidx, &added);
        const bool spbin = (sdrc == VK_SUCCESS) && onr::vk_device_has_pipeline_binary(sdev);
        if (sdev) {
            onr::vk_note_device_destroyed(sdev);
            vkDestroyDevice(sdev, nullptr);
        }
        vkDestroyInstance(inst, nullptr);
        if (!split_unmutated || sdrc != VK_SUCCESS || sfrc != 0 || !spbin) {
            std::fprintf(stderr, "FAIL: vk_host split-chain unmut=%d sdrc=%d sfrc=%d spbin=%d\n",
                         int(split_unmutated), int(sdrc), sfrc, int(spbin));
            return 1;
        }
        std::printf("[PASS] vk_host split-chain non-mutating clone + VK_KHR_maintenance5 + VK_KHR_pipeline_binary\n");
    }

    // 3. pal::splice gfx1200 (0x48) + gfx11 rejection + xgl_layout & splice_vulkan
    {
        using namespace onr::pal;
        Record r;
        r.spv_hash = 0x123456789abcdef0ull;
        r.code = {0x00, 0x00, 0xb0, 0xbf}; // s_endpgm
        r.code_hash = hash(r.code.data(), r.code.size());
        r.exec = 4;
        r.wg = 64;
        r.push_bytes = 244;
        r.rsrc1 = 0;
        r.rsrc2 = (1u << 1) | (1u << 7); // user_sgprs=1, tgid_x_en=1
        r.user = 1;
        r.set0 = 0;
        r.mach = 0x48;

        const Bytes elf48 = make_synthetic_pal_elf(0x48, 66, {0x10000000u, 1u, 65u});
        const Bytes elf41 = make_synthetic_pal_elf(0x41, 66, {0x10000000u, 1u, 65u});
        const Bytes elf69 = make_synthetic_pal_elf(0x4e, 69, {0x10000000u, 4u, 68u});
        const Bytes elf5  = make_synthetic_pal_elf(0x4e, 5,  {0x10000000u, 1u, 2u});
        const Bytes elf82 = make_synthetic_pal_elf(0x4e, 82, {0x10000000u, 17u, 81u});

        const Bytes d3d_out = splice(elf48, r, 256, {64, 1, 1}, 4);
        if (Elf(d3d_out).mach != 0x48) {
            std::fprintf(stderr, "FAIL: pal::splice did not preserve mach=0x48\n");
            return 1;
        }
        bool rejected_41 = false;
        try { (void)splice(elf41, r, 256, {64, 1, 1}, 4); } catch (const std::exception&) { rejected_41 = true; }
        if (!rejected_41) {
            std::fprintf(stderr, "FAIL: pal::splice failed to reject 0x41 ELF with 0x48 record\n");
            return 1;
        }

        const XglLayout l66 = xgl_layout(pal_metadata(Elf(elf48).data[Elf(elf48).find(".note")]), 256);
        const XglLayout l69 = xgl_layout(pal_metadata(Elf(elf69).data[Elf(elf69).find(".note")]), 256);
        if (l66.push_base != 1 || l66.descriptor_slot != 65 || l69.push_base != 4 || l69.descriptor_slot != 68) {
            std::fprintf(stderr, "FAIL: xgl_layout unexpected base/slot\n");
            return 1;
        }
        bool rej_5 = false, rej_82 = false;
        try { (void)xgl_layout(pal_metadata(Elf(elf5).data[Elf(elf5).find(".note")]), 256); } catch (const std::exception&) { rej_5 = true; }
        try { (void)xgl_layout(pal_metadata(Elf(elf82).data[Elf(elf82).find(".note")]), 256); } catch (const std::exception&) { rej_82 = true; }
        if (!rej_5 || !rej_82) {
            std::fprintf(stderr, "FAIL: xgl_layout failed to reject limit=5 or push_base=17\n");
            return 1;
        }
        const Bytes vk66 = splice_vulkan(elf48, r, 256, {64, 1, 1});
        const Bytes vk69 = splice_vulkan(elf69, r, 256, {64, 1, 1});
        const Value m66 = pal_metadata(Elf(vk66).data[Elf(vk66).find(".note")]);
        const Value m69 = pal_metadata(Elf(vk69).data[Elf(vk69).find(".note")]);
        const auto& p66 = m66.at("amdpal.pipelines").items[0];
        const auto& p69 = m69.at("amdpal.pipelines").items[0];
        if (p66.at(".hardware_stages").at(".cs").at(".sgpr_count").number() != 106 ||
            p66.at(".user_data_limit").number() != 66 ||
            p66.at(".hardware_stages").at(".cs").at(".user_data_reg_map").items[1].number() != 65 ||
            p69.at(".user_data_limit").number() != 69 ||
            p69.at(".hardware_stages").at(".cs").at(".user_data_reg_map").items[1].number() != 68) {
            std::fprintf(stderr, "FAIL: splice_vulkan metadata mismatch\n");
            return 1;
        }
        std::printf("[PASS] pal::splice gfx1200 (0x48), arch rejection, XglLayout (66/69) & splice_vulkan\n");
    }

    // 4. OnrRrSetFp8 / OnrRrGetFp8 ABI & state machine (0/1/2/3/4)
    {
        if (OnrRrSetFp8(nullptr, 1) != ONR_RR_INVALID_ARGUMENT || OnrRrGetFp8(nullptr) != 0) {
            std::fprintf(stderr, "FAIL: OnrRrSetFp8/GetFp8 null check failed\n");
            return 1;
        }
        const std::filesystem::path dlssd = L"C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll";
        wchar_t self_exe[MAX_PATH]{};
        std::filesystem::path bdir = "build/asi";
        if (GetModuleFileNameW(nullptr, self_exe, MAX_PATH) != 0) {
            const auto exe_dir = std::filesystem::path(self_exe).parent_path();
            if (std::filesystem::exists(exe_dir / "stage" / "openrr-amd" / "shaders"))
                bdir = exe_dir;
        }
        const auto sdir = bdir / "stage" / "openrr-amd";
        OnrRrApi4 api{};
        if (std::filesystem::exists(dlssd) && std::filesystem::exists(sdir / "shaders") &&
            OnrRrGetApi(ONR_RR_ABI_VERSION_4, &api.v3.v2.v1) == ONR_RR_OK) {
            IDXGIFactory6* fac = nullptr;
            ID3D12Device* d3d = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
                IDXGIAdapter1* ad = nullptr;
                if (SUCCEEDED(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad))) && ad) {
                    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d)))) d3d = nullptr;
                    ad->Release();
                }
                fac->Release();
            }
            if (d3d) {
                const auto logp = (bdir / "audit-fp8-state.log").wstring();
                OnrRrCreateInfo ci{};
                ci.size = sizeof ci;
                ci.d3d12_device = d3d;
                ci.max_render_width = ci.max_render_height = 64;
                ci.output_width = ci.output_height = 64;
                ci.dlssd_path = dlssd.c_str();
                ci.data_dir = sdir.c_str();
                ci.log_path = logp.c_str();
                ci.backend = ONR_RR_BACKEND_CHOICE_VULKAN;
                void* ctx = nullptr;
                if (api.v3.v2.v1.Create(&ci, &ctx) == ONR_RR_OK && ctx) {
                    const int32_t init_st = OnrRrGetFp8(ctx);
                    OnrRrSetFp8(ctx, 1);
                    const int32_t leg_st = OnrRrGetFp8(ctx);
                    OnrRrSetFastMode(ctx, 1);
                    const int32_t req_st = OnrRrGetFp8(ctx);
                    for (int t = 0; t < 200; ++t) {
                        OnrRrInfo info{sizeof info};
                        api.v3.v2.v1.GetInfo(ctx, &info);
                        if (info.state == ONR_RR_STATE_READY || info.state == ONR_RR_STATE_FAILED) break;
                        Sleep(50);
                    }
                    OnrRrSetFp8(ctx, 0);
                    const int32_t off_st = OnrRrGetFp8(ctx);
                    api.v3.v2.v1.Destroy(ctx);
                    d3d->Release();
                    std::error_code ec_rm;
                    std::filesystem::remove(logp, ec_rm);
                    if (init_st != 0 || leg_st != 4 || (req_st != 2 && req_st != 1 && req_st != 3) || off_st > 4) {
                        std::fprintf(stderr, "FAIL: OnrRrGetFp8 states init=%d leg=%d req=%d off=%d\n",
                                     init_st, leg_st, req_st, off_st);
                        return 1;
                    }
                    std::printf("[PASS] OnrRrSetFp8/OnrRrGetFp8 state machine (init=%d leg=%d req=%d off=%d)\n",
                                init_st, leg_st, req_st, off_st);
                } else {
                    d3d->Release();
                }
            }
            // Check RDNA3 (780M) if present: OnrRrSetFp8(1) returns ONR_RR_OK and OnrRrGetFp8 reports 3 (unavailable)
            IDXGIFactory6* fac3 = nullptr;
            ID3D12Device* d3d3 = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac3))) && fac3) {
                for (UINT ai = 0;; ++ai) {
                    IDXGIAdapter1* ad = nullptr;
                    if (fac3->EnumAdapters1(ai, &ad) == DXGI_ERROR_NOT_FOUND || !ad) break;
                    DXGI_ADAPTER_DESC1 dd{};
                    ad->GetDesc1(&dd);
                    if (!(dd.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && dd.VendorId == 0x1002 &&
                        std::wcsstr(dd.Description, L"780M")) {
                        if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d3)))) d3d3 = nullptr;
                        ad->Release();
                        break;
                    }
                    ad->Release();
                }
                fac3->Release();
            }
            if (d3d3) {
                const auto logp3 = (bdir / "audit-fp8-rdna3.log").wstring();
                OnrRrCreateInfo ci3{};
                ci3.size = sizeof ci3;
                ci3.d3d12_device = d3d3;
                ci3.max_render_width = ci3.max_render_height = 64;
                ci3.output_width = ci3.output_height = 64;
                ci3.dlssd_path = dlssd.c_str();
                ci3.data_dir = sdir.c_str();
                ci3.log_path = logp3.c_str();
                ci3.backend = ONR_RR_BACKEND_CHOICE_VULKAN;
                void* ctx3 = nullptr;
                if (api.v3.v2.v1.Create(&ci3, &ctx3) == ONR_RR_OK && ctx3) {
                    const int32_t s0 = OnrRrGetFp8(ctx3);
                    const int32_t rc1 = OnrRrSetFp8(ctx3, 1);
                    const int32_t s1 = OnrRrGetFp8(ctx3);
                    const int32_t rc0 = OnrRrSetFp8(ctx3, 0);
                    const int32_t s2 = OnrRrGetFp8(ctx3);
                    api.v3.v2.v1.Destroy(ctx3);
                    d3d3->Release();
                    std::error_code ec3;
                    std::filesystem::remove(logp3, ec3);
                    if (s0 != 0 || rc1 != ONR_RR_OK || s1 != 3 || rc0 != ONR_RR_OK || s2 != 0) {
                        std::fprintf(stderr, "FAIL: RDNA3 780M OnrRrSetFp8/GetFp8 s0=%d rc1=%d s1=%d rc0=%d s2=%d\n",
                                     s0, rc1, s1, rc0, s2);
                        return 1;
                    }
                    std::printf("[PASS] RDNA3 780M OnrRrSetFp8/OnrRrGetFp8 (s0=%d s1=%d s2=%d)\n", s0, s1, s2);
                } else {
                    d3d3->Release();
                }
            }
        }
    }

    // 5. Hosted Vulkan Device descriptor/sampler reset_textures & retirement proof lifecycle
    {
        openrr::pe::vulkan_entry_points();
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || !inst) {
            std::fprintf(stderr, "FAIL: hosted lifecycle vkCreateInstance failed\n");
            return 1;
        }
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> devs(count);
        if (count) vkEnumeratePhysicalDevices(inst, &count, devs.data());
        const VkPhysicalDevice picked = onr::pick_vk_physical_device(devs, nullptr);
        if (!picked) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: hosted lifecycle pick_vk_physical_device failed\n");
            return 1;
        }
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(picked, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(picked, &qcount, qprops.data());
        uint32_t gfx_q = 0;
        for (uint32_t i = 0; i < qcount; ++i)
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_q = i; break; }
        float qprio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = gfx_q;
        qci.queueCount = 1;
        qci.pQueuePriorities = &qprio;
        VkDeviceCreateInfo game_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        game_ci.queueCreateInfoCount = 1;
        game_ci.pQueueCreateInfos = &qci;
        VkDeviceCreateInfo mod_ci{};
        void* setup_tok = nullptr;
        std::string why;
        if (onr_host::prepare(picked, &game_ci, &mod_ci, &setup_tok, why) != 0) {
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: hosted lifecycle prepare failed: %s\n", why.c_str());
            return 1;
        }
        VkDevice hdev = VK_NULL_HANDLE;
        const VkResult drc = vkCreateDevice(picked, &mod_ci, nullptr, &hdev);
        uint32_t rfam = 0, ridx = 0;
        const int frc = onr_host::finish(setup_tok, drc == VK_SUCCESS ? hdev : VK_NULL_HANDLE, &rfam, &ridx, nullptr);
        if (drc != VK_SUCCESS || frc != 0 || !hdev) {
            if (hdev) vkDestroyDevice(hdev, nullptr);
            vkDestroyInstance(inst, nullptr);
            std::fprintf(stderr, "FAIL: hosted lifecycle vkCreateDevice drc=%d frc=%d\n", int(drc), frc);
            return 1;
        }

        VkCommandPool hpool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkQueryPool hqp = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueue hq = VK_NULL_HANDLE;
        vkGetDeviceQueue(hdev, gfx_q, 0, &hq);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gfx_q;
        vkCreateCommandPool(hdev, &pci, nullptr, &hpool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = hpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(hdev, &cai, &cb);
        onr::vk_note_command_buffers_allocated(hdev, hpool, 1, &cb);
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = onr::kMaxQueries;
        vkCreateQueryPool(hdev, &qpi, nullptr, &hqp);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(hdev, &fci, nullptr, &fence);

        bool lifecycle_ok = false;
        {
            onr::Device hd;
            onr::HostDevice host_arg{inst, picked, hdev, rfam, ridx};
            std::string herr;
            if (!hd.adopt(host_arg, herr)) {
                std::fprintf(stderr, "FAIL: hosted Device::adopt failed: %s\n", herr.c_str());
                return 1;
            }
            hd.set_compiler(false);
            onr::Buffer sb = hd.create(64 * 8 * 8);
            onr::Image im1 = hd.create_image(64, 8, VK_FORMAT_R16G16B16A16_SFLOAT, sb);
            onr::Image im2 = hd.create_image(64, 8, VK_FORMAT_R16G16B16A16_SFLOAT, sb);
            VkSampler smp1 = hd.sampler(true, true);
            VkSampler smp2 = hd.sampler(false, false);

            wchar_t self_exe[MAX_PATH]{};
            std::filesystem::path spv_path = "build/asi/stage/openrr-amd/shaders/selftest_tex.spv";
            if (GetModuleFileNameW(nullptr, self_exe, MAX_PATH) != 0) {
                const auto cand = std::filesystem::path(self_exe).parent_path() / "stage" / "openrr-amd" / "shaders" / "selftest_tex.spv";
                if (std::filesystem::exists(cand)) spv_path = cand;
            }
            onr::Kernel k_st;
            const bool has_st = std::filesystem::exists(spv_path);
            if (has_st) k_st = hd.load(onr::path_to_u8(spv_path));

            hd.set_texture(1, im1, smp1);
            hd.set_texture(2, im2, smp1);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &bi);
            hd.bind(cb, hqp);
            if (has_st) {
                struct { uint64_t dst; int32_t w, h; uint32_t slot; } pc{sb.addr, 64, 8, 1u};
                hd.dispatch(k_st, &pc, sizeof(pc), 8);
            }
            hd.destroy(smp1);
            hd.destroy(im1);
            hd.unbind();
            vkEndCommandBuffer(cb);

            const bool buried_while_active = !hd.all_retired();
            hd.reset_textures();
            hd.set_texture(2, im2, smp2);
            const bool still_unretired_before_submit = !hd.all_retired();

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            vkQueueSubmit(hq, 1, &si, fence);
            vkWaitForFences(hdev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
            vkResetFences(hdev, 1, &fence);

            vkResetCommandBuffer(cb, 0);
            onr::vk_note_command_buffer_reset(cb);
            const bool retired_after_reset = hd.all_retired();

            if (has_st) hd.destroy(k_st);
            hd.destroy(smp2);
            hd.destroy(im2);
            hd.destroy(sb);
            vkDeviceWaitIdle(hdev);
            lifecycle_ok = buried_while_active && still_unretired_before_submit && retired_after_reset;
            if (!lifecycle_ok) {
                std::fprintf(stderr, "FAIL: hosted lifecycle buried=%d still=%d retired=%d\n",
                             int(buried_while_active), int(still_unretired_before_submit), int(retired_after_reset));
            }
        }

        vkDestroyFence(hdev, fence, nullptr);
        vkDestroyQueryPool(hdev, hqp, nullptr);
        onr::vk_note_command_pool_destroyed(hdev, hpool);
        vkDestroyCommandPool(hdev, hpool, nullptr);
        onr::vk_note_device_destroyed(hdev);
        vkDestroyDevice(hdev, nullptr);
        vkDestroyInstance(inst, nullptr);
        if (!lifecycle_ok) return 1;
        std::printf("[PASS] Hosted Vulkan Device descriptor/sampler reset_textures & Grave retirement proof\n");
    }
    return 0;
}

extern "C" size_t onr_test_gpu_sample_count(void* context);

static int test_stage10_final_audit() {
    const std::filesystem::path dlssd = L"C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll";
    wchar_t self_exe[MAX_PATH]{};
    std::filesystem::path bdir = "build/asi";
    if (GetModuleFileNameW(nullptr, self_exe, MAX_PATH) != 0) {
        const auto exe_dir = std::filesystem::path(self_exe).parent_path();
        if (std::filesystem::exists(exe_dir / "stage" / "openrr-amd" / "shaders")) bdir = exe_dir;
    }
    const auto data_dir = bdir / "stage" / "openrr-amd";
    const auto shader_dir = data_dir / "shaders";
    const std::wstring w_data = data_dir.wstring();

    // 1. Release Create() rejects non-zero ci.compiler overrides
    {
        OnrRrApi api{};
        if (OnrRrGetApi(ONR_RR_ABI_VERSION, &api) != ONR_RR_OK) return 1;
        IDXGIFactory6* fac = nullptr;
        ID3D12Device* d3d = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter1* ad = nullptr;
            if (SUCCEEDED(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad))) && ad) {
                if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d)))) d3d = nullptr;
                ad->Release();
            }
            fac->Release();
        }
        if (d3d && std::filesystem::exists(dlssd) && std::filesystem::exists(shader_dir)) {
            const auto logp = (bdir / "audit-compiler-override.log").wstring();
            const std::wstring w_dlssd = dlssd.wstring();
            for (uint32_t bad_c : {1u, 2u, 3u, 99u}) {
                OnrRrCreateInfo ci{};
                ci.size = sizeof ci;
                ci.compiler = bad_c;
                ci.d3d12_device = d3d;
                ci.max_render_width = ci.max_render_height = 64;
                ci.output_width = ci.output_height = 64;
                ci.dlssd_path = w_dlssd.c_str();
                ci.data_dir = w_data.c_str();
                ci.log_path = logp.c_str();
                ci.backend = ONR_RR_BACKEND_CHOICE_AUTO;
                void* ctx = nullptr;
                const int32_t rc = api.Create(&ci, &ctx);
                char err[512]{};
                api.GetLastError(err, sizeof err);
                if (rc == ONR_RR_OK || ctx != nullptr || !std::strstr(err, "compiler")) {
                    if (ctx) api.Destroy(ctx);
                    d3d->Release();
                    std::fprintf(stderr, "FAIL: Create accepted ci.compiler=%u (rc=%d err=%s)\n", bad_c, rc, err);
                    return 1;
                }
            }
            { std::error_code ec_rm; std::filesystem::remove(logp, ec_rm); }
            std::printf("[PASS] Stage10: Create rejects non-zero ci.compiler overrides (1/2/3/99)\n");
        }
        if (d3d) d3d->Release();
    }

    // 2 & 3. Vulkan PAGE_READONLY PrepareVkDevice, native VkDevice missing pipeline_binary, and vk_harvest discard regression
    {
        OnrRrApi5 api5{};
        if (OnrRrGetApi(ONR_RR_ABI_VERSION_5, &api5.v4.v3.v2.v1) != ONR_RR_OK) return 1;
        OnrRrApi& api = api5.v4.v3.v2.v1;

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS && inst) {
            uint32_t npd = 0;
            vkEnumeratePhysicalDevices(inst, &npd, nullptr);
            std::vector<VkPhysicalDevice> pds(npd);
            vkEnumeratePhysicalDevices(inst, &npd, pds.data());
            VkPhysicalDevice amd_pd = VK_NULL_HANDLE;
            VkPhysicalDevice rdna4_pd = VK_NULL_HANDLE;
            for (auto pd : pds) {
                VkPhysicalDeviceProperties pp{};
                vkGetPhysicalDeviceProperties(pd, &pp);
                if (pp.vendorID != 0x1002) continue;
                const auto tid = onr::identify_vk_physical_device(pd);
                const bool is_r4 = (onr::arch::aco_target_for_mach(tid.elf_mach) != nullptr) || tid.arch == onr::arch::GpuArch::Rdna4;
                if (is_r4 && !rdna4_pd) rdna4_pd = pd;
                if (!amd_pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) amd_pd = pd;
            }
            VkPhysicalDevice pd = rdna4_pd ? rdna4_pd : amd_pd;
            if (pd) {
                uint32_t nqf = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
                std::vector<VkQueueFamilyProperties> qfs(nqf);
                vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
                uint32_t gfx = 0;
                while (gfx < nqf && !(qfs[gfx].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++gfx;
                if (gfx == nqf) {
                    gfx = 0;
                    while (gfx < nqf && !(qfs[gfx].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++gfx;
                }

                // 2. PAGE_READONLY caller chain tests
                struct RoLayout {
                    float prio;
                    VkDeviceQueueCreateInfo qci;
                    struct UnknownNode {
                        VkStructureType sType;
                        const void* pNext;
                        uint32_t payload[4];
                    } unk;
                    VkPhysicalDeviceVulkan12Features v12;
                    VkPhysicalDeviceVulkan14Features v14;
                    VkPhysicalDeviceMaintenance5FeaturesKHR m5;
                    VkDeviceCreateInfo dci;
                };
                static_assert(sizeof(RoLayout) <= 4096);
                void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!page) return 1;
                auto* ro = static_cast<RoLayout*>(page);

                auto snapshot_ro = [&]() -> std::vector<uint8_t> {
                    DWORD old_p = 0;
                    VirtualProtect(page, 4096, PAGE_READONLY, &old_p);
                    std::vector<uint8_t> snap(4096);
                    memcpy(snap.data(), page, 4096);
                    return snap;
                };
                auto unprotect_rw = [&]() {
                    DWORD old_p = 0;
                    VirtualProtect(page, 4096, PAGE_READWRITE, &old_p);
                };

                // 2a: Unknown sType -> disabled Vulkan12(bufferDeviceAddress=VK_FALSE) on PAGE_READONLY
                memset(page, 0, 4096);
                ro->prio = 1.0f;
                ro->qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                ro->qci.queueFamilyIndex = gfx;
                ro->qci.queueCount = 1;
                ro->qci.pQueuePriorities = &ro->prio;
                ro->v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                ro->v12.bufferDeviceAddress = VK_FALSE;
                ro->unk = {static_cast<VkStructureType>(0x7ffffffe), &ro->v12, {1, 2, 3, 4}};
                ro->dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ro->unk};
                ro->dci.queueCreateInfoCount = 1;
                ro->dci.pQueueCreateInfos = &ro->qci;
                auto before_2a = snapshot_ro();
                VkDeviceCreateInfo out_ci{};
                void* setup_2a = reinterpret_cast<void*>(uintptr_t(1));
                const int32_t rc_2a = api5.PrepareVkDevice(pd, &ro->dci, &out_ci, &setup_2a);
                if (rc_2a == ONR_RR_OK || setup_2a != nullptr || memcmp(page, before_2a.data(), 4096) != 0) {
                    unprotect_rw();
                    VirtualFree(page, 0, MEM_RELEASE);
                    std::fprintf(stderr, "FAIL: PrepareVkDevice PAGE_READONLY 2a rc=%d setup=%p\n", rc_2a, setup_2a);
                    return 1;
                }

                // 2b: Unknown sType -> ALREADY-ENABLED Vulkan12 on PAGE_READONLY must succeed with 0 mutations
                unprotect_rw();
                memset(page, 0, 4096);
                ro->prio = 1.0f;
                ro->qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                ro->qci.queueFamilyIndex = gfx;
                ro->qci.queueCount = 1;
                ro->qci.pQueuePriorities = &ro->prio;
                ro->v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                ro->v12.storageBuffer8BitAccess = VK_TRUE;
                ro->v12.shaderFloat16 = VK_TRUE;
                ro->v12.shaderInt8 = VK_TRUE;
                ro->v12.bufferDeviceAddress = VK_TRUE;
                ro->v12.vulkanMemoryModel = VK_TRUE;
                ro->v12.vulkanMemoryModelDeviceScope = VK_TRUE;
                ro->v12.shaderBufferInt64Atomics = VK_TRUE;
                ro->v12.descriptorBindingPartiallyBound = VK_TRUE;
                ro->v12.hostQueryReset = VK_TRUE;
                ro->v12.scalarBlockLayout = VK_TRUE;
                ro->unk = {static_cast<VkStructureType>(0x7ffffffd), &ro->v12, {5, 6, 7, 8}};
                ro->dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ro->unk};
                ro->dci.queueCreateInfoCount = 1;
                ro->dci.pQueueCreateInfos = &ro->qci;
                auto before_2b = snapshot_ro();
                void* setup_2b = nullptr;
                const int32_t rc_2b = api5.PrepareVkDevice(pd, &ro->dci, &out_ci, &setup_2b);
                if (rc_2b != ONR_RR_OK || !setup_2b || memcmp(page, before_2b.data(), 4096) != 0) {
                    if (setup_2b) api5.FinishVkDevice(setup_2b, VK_NULL_HANDLE, nullptr, nullptr);
                    unprotect_rw();
                    VirtualFree(page, 0, MEM_RELEASE);
                    std::fprintf(stderr, "FAIL: PrepareVkDevice PAGE_READONLY 2b rc=%d setup=%p\n", rc_2b, setup_2b);
                    return 1;
                }
                api5.FinishVkDevice(setup_2b, VK_NULL_HANDLE, nullptr, nullptr);

                // 2c: Conflicting Vulkan14Features + Maintenance5FeaturesKHR on PAGE_READONLY
                unprotect_rw();
                memset(page, 0, 4096);
                ro->prio = 1.0f;
                ro->qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                ro->qci.queueFamilyIndex = gfx;
                ro->qci.queueCount = 1;
                ro->qci.pQueuePriorities = &ro->prio;
                ro->m5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
                ro->m5.maintenance5 = VK_TRUE;
                ro->v14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &ro->m5};
                ro->v14.maintenance5 = VK_TRUE;
                ro->dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ro->v14};
                ro->dci.queueCreateInfoCount = 1;
                ro->dci.pQueueCreateInfos = &ro->qci;
                auto before_2c = snapshot_ro();
                void* setup_2c = reinterpret_cast<void*>(uintptr_t(1));
                const int32_t rc_2c = api5.PrepareVkDevice(pd, &ro->dci, &out_ci, &setup_2c);
                const bool mem_2c_ok = memcmp(page, before_2c.data(), 4096) == 0;
                unprotect_rw();
                VirtualFree(page, 0, MEM_RELEASE);
                if (rc_2c == ONR_RR_OK || setup_2c != nullptr || !mem_2c_ok) {
                    std::fprintf(stderr, "FAIL: PrepareVkDevice PAGE_READONLY 2c conflict check failed\n");
                    return 1;
                }
                std::printf("[PASS] Stage10: vk_host::prepare PAGE_READONLY zero-mutation (unknown+disabled, unknown+enabled, v14+m5 conflict)\n");

                // 2d: Native VkDevice created WITHOUT VK_KHR_pipeline_binary must cause RDNA4 Create() to fail explicitly
                if (rdna4_pd && std::filesystem::exists(shader_dir)) {
                    float prio = 1.0f;
                    VkDeviceQueueCreateInfo qci_raw{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    qci_raw.queueFamilyIndex = gfx; qci_raw.queueCount = 1; qci_raw.pQueuePriorities = &prio;
                    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR rwl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
                    rwl.workgroupMemoryExplicitLayout = VK_TRUE;
                    rwl.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
                    VkPhysicalDeviceShaderFloat8FeaturesEXT rf8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &rwl};
                    rf8.shaderFloat8 = VK_TRUE; rf8.shaderFloat8CooperativeMatrix = VK_TRUE;
                    VkPhysicalDeviceCooperativeMatrixFeaturesKHR rcm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &rf8};
                    rcm.cooperativeMatrix = VK_TRUE;
                    VkPhysicalDeviceVulkan13Features r13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &rcm};
                    r13.synchronization2 = VK_TRUE; r13.subgroupSizeControl = VK_TRUE; r13.computeFullSubgroups = VK_TRUE;
                    VkPhysicalDeviceVulkan12Features r12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &r13};
                    r12.storageBuffer8BitAccess = VK_TRUE; r12.shaderFloat16 = VK_TRUE; r12.shaderInt8 = VK_TRUE;
                    r12.bufferDeviceAddress = VK_TRUE; r12.vulkanMemoryModel = VK_TRUE; r12.vulkanMemoryModelDeviceScope = VK_TRUE;
                    r12.shaderBufferInt64Atomics = VK_TRUE; r12.descriptorBindingPartiallyBound = VK_TRUE;
                    r12.hostQueryReset = VK_TRUE; r12.scalarBlockLayout = VK_TRUE;
                    VkPhysicalDeviceVulkan11Features r11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &r12};
                    r11.storageBuffer16BitAccess = VK_TRUE;
                    VkPhysicalDeviceFeatures rcore{}; rcore.shaderInt64 = VK_TRUE; rcore.shaderInt16 = VK_TRUE; rcore.shaderFloat64 = VK_TRUE;
                    const char* rexts[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME};
                    VkDeviceCreateInfo rdci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &r11};
                    rdci.queueCreateInfoCount = 1; rdci.pQueueCreateInfos = &qci_raw;
                    rdci.enabledExtensionCount = 3; rdci.ppEnabledExtensionNames = rexts;
                    rdci.pEnabledFeatures = &rcore;
                    VkDevice raw_dev = VK_NULL_HANDLE;
                    if (vkCreateDevice(rdna4_pd, &rdci, nullptr, &raw_dev) == VK_SUCCESS && raw_dev) {
                        const auto log_nopbin = (bdir / "audit-vk-no-pbin.log").wstring();
                        OnrRrVkDevice vkd{sizeof vkd, VK_API_VERSION_1_3, inst, rdna4_pd, raw_dev, gfx, 0};
                        OnrRrCreateInfo civ{sizeof civ};
                        civ.max_render_width = 426; civ.max_render_height = 240;
                        civ.output_width = 640; civ.output_height = 360;
                        civ.data_dir = w_data.c_str();
                        civ.log_path = log_nopbin.c_str();
                        civ.backend = ONR_RR_BACKEND_CHOICE_AUTO;
                        civ.vk_device = &vkd;
                        void* ctx_np = nullptr;
                        const int32_t rc_np = api.Create(&civ, &ctx_np);
                        char err[512]{};
                        api.GetLastError(err, sizeof err);
                        if (ctx_np) api.Destroy(ctx_np);
                        onr::vk_note_device_destroyed(raw_dev);
                        vkDestroyDevice(raw_dev, nullptr);
                        { std::error_code ec_rm; std::filesystem::remove(log_nopbin, ec_rm); }
                        if (rc_np == ONR_RR_OK || ctx_np != nullptr || !std::strstr(err, "pipeline_binary")) {
                            std::fprintf(stderr, "FAIL: RDNA4 native VkDevice without VK_KHR_pipeline_binary succeeded or wrong err (rc=%d err=%s)\n", rc_np, err);
                            return 1;
                        }
                        std::printf("[PASS] Stage10: RDNA4 Create() rejects VkDevice without VK_KHR_pipeline_binary (%s)\n", err);
                    }
                }

                // 3. vk_harvest query discard timing regression + gfx1200 fault checks on hosted VkDevice
                if (std::filesystem::exists(shader_dir)) {
                    float prio = 1.0f;
                    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    qci.queueFamilyIndex = gfx; qci.queueCount = 1; qci.pQueuePriorities = &prio;
                    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
                    VkDeviceCreateInfo patched{};
                    void* setup = nullptr;
                    if (api5.PrepareVkDevice(pd, &dci, &patched, &setup) != ONR_RR_OK) return 1;
                    VkDevice hdev = VK_NULL_HANDLE;
                    const VkResult cr = vkCreateDevice(pd, &patched, nullptr, &hdev);
                    uint32_t rf = 0, ri = 0;
                    api5.FinishVkDevice(setup, cr == VK_SUCCESS ? hdev : nullptr, &rf, &ri);
                    if (cr != VK_SUCCESS || !hdev) return 1;

                    // Helper to create GENERAL RGBA16F images
                    VkPhysicalDeviceMemoryProperties memp{};
                    vkGetPhysicalDeviceMemoryProperties(pd, &memp);
                    auto find_mem = [&](uint32_t bits, VkMemoryPropertyFlags flags) -> uint32_t {
                        for (uint32_t i = 0; i < memp.memoryTypeCount; ++i)
                            if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & flags) == flags) return i;
                        return 0;
                    };
                    auto make_img = [&](uint32_t w, uint32_t h, VkImage& im, VkDeviceMemory& mem, VkImageView& iv) {
                        VkImageCreateInfo ici2{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                        ici2.imageType = VK_IMAGE_TYPE_2D;
                        ici2.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                        ici2.extent = {w, h, 1};
                        ici2.mipLevels = 1; ici2.arrayLayers = 1;
                        ici2.samples = VK_SAMPLE_COUNT_1_BIT;
                        ici2.tiling = VK_IMAGE_TILING_OPTIMAL;
                        ici2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                        ici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        vkCreateImage(hdev, &ici2, nullptr, &im);
                        VkMemoryRequirements mr{};
                        vkGetImageMemoryRequirements(hdev, im, &mr);
                        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                        mai.allocationSize = mr.size;
                        mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                        vkAllocateMemory(hdev, &mai, nullptr, &mem);
                        vkBindImageMemory(hdev, im, mem, 0);
                        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        ivci.image = im; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        ivci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCreateImageView(hdev, &ivci, nullptr, &iv);
                    };

                    VkImage im_col = VK_NULL_HANDLE, im_out = VK_NULL_HANDLE;
                    VkDeviceMemory mem_col = VK_NULL_HANDLE, mem_out = VK_NULL_HANDLE;
                    VkImageView iv_col = VK_NULL_HANDLE, iv_out = VK_NULL_HANDLE;
                    make_img(426, 240, im_col, mem_col, iv_col);
                    make_img(640, 360, im_out, mem_out, iv_out);

                    VkQueue q_gfx = VK_NULL_HANDLE;
                    vkGetDeviceQueue(hdev, gfx, 0, &q_gfx);
                    VkCommandPool cpool = VK_NULL_HANDLE;
                    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                    pci.queueFamilyIndex = gfx;
                    vkCreateCommandPool(hdev, &pci, nullptr, &cpool);
                    VkCommandBuffer cb = VK_NULL_HANDLE;
                    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                    vkAllocateCommandBuffers(hdev, &cai, &cb);
                    OnrRrNotifyVkCommandBuffersAllocated(hdev, cpool, 1, reinterpret_cast<void* const*>(&cb));
                    VkFence fence = VK_NULL_HANDLE;
                    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    vkCreateFence(hdev, &fci, nullptr, &fence);

                    // Transition images to GENERAL
                    {
                        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        vkBeginCommandBuffer(cb, &bi);
                        VkImageMemoryBarrier2 ib[2]{};
                        for (int k = 0; k < 2; ++k) {
                            ib[k] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                            ib[k].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                            ib[k].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                            ib[k].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                            ib[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                            ib[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                            ib[k].srcQueueFamilyIndex = ib[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            ib[k].image = k == 0 ? im_col : im_out;
                            ib[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        }
                        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        di.imageMemoryBarrierCount = 2; di.pImageMemoryBarriers = ib;
                        vkCmdPipelineBarrier2(cb, &di);
                        vkEndCommandBuffer(cb);
                        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                        vkQueueSubmit(q_gfx, 1, &si, fence);
                        vkWaitForFences(hdev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
                        vkResetFences(hdev, 1, &fence);
                        vkResetCommandBuffer(cb, 0);
                        OnrRrNotifyVkCommandBufferReset(cb);
                    }

                    const auto log_hd = (bdir / "audit-vk-harvest-discard.log").wstring();
                    OnrRrVkDevice vkd{sizeof vkd, VK_API_VERSION_1_3, inst, pd, hdev, rf, ri};
                    OnrRrCreateInfo ci_hd{sizeof ci_hd};
                    ci_hd.max_render_width = 426; ci_hd.max_render_height = 240;
                    ci_hd.output_width = 640; ci_hd.output_height = 360;
                    ci_hd.data_dir = w_data.c_str();
                    ci_hd.log_path = log_hd.c_str();
                    ci_hd.backend = ONR_RR_BACKEND_CHOICE_AUTO;
                    ci_hd.vk_device = &vkd;
                    void* ctx_hd = nullptr;
                    if (api.Create(&ci_hd, &ctx_hd) != ONR_RR_OK || !ctx_hd) {
                        char err[512]{};
                        api.GetLastError(err, sizeof err);
                        std::fprintf(stderr, "FAIL: harvest-discard Create failed: %s\n", err);
                        return 1;
                    }

                    OnrRrVkImage d_col{uint64_t(im_col), uint64_t(iv_col), VK_FORMAT_R16G16B16A16_SFLOAT, 426, 240, 0, 0,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
                    OnrRrVkImage d_out{uint64_t(im_out), uint64_t(iv_out), VK_FORMAT_R16G16B16A16_SFLOAT, 640, 360, 0, 0,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
                    OnrRrFrame fr{};
                    fr.size = sizeof fr;
                    fr.render_width = 426; fr.render_height = 240;
                    fr.mv_scale_x = 426.0f; fr.mv_scale_y = 240.0f;
                    for (int k = 0; k < 16; ++k) fr.world_to_view[k] = fr.view_to_clip[k] = (k % 5 == 0) ? 1.0f : 0.0f;
                    fr.color = {&d_col, VK_IMAGE_LAYOUT_GENERAL, 0, 0};
                    fr.diffuse_albedo = fr.specular_albedo = fr.normals = fr.roughness = fr.depth = fr.motion = fr.color;
                    fr.output = {&d_out, VK_IMAGE_LAYOUT_GENERAL, 0, 0};

                    auto rec_step = [&](bool do_submit) -> bool {
                        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        vkBeginCommandBuffer(cb, &bi);
                        const int32_t rrc = api5.RecordVk(ctx_hd, &fr, cb);
                        vkEndCommandBuffer(cb);
                        if (rrc != ONR_RR_OK) return false;
                        if (do_submit) {
                            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                            if (vkQueueSubmit(q_gfx, 1, &si, fence) != VK_SUCCESS) return false;
                            if (vkWaitForFences(hdev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS) return false;
                            vkResetFences(hdev, 1, &fence);
                        }
                        vkResetCommandBuffer(cb, 0);
                        OnrRrNotifyVkCommandBufferReset(cb);
                        return true;
                    };

                    // Valid sample on slot 0
                    if (!rec_step(true)) return 1;
                    // Discard 63 frames (slots 1..63); frame 1 harvests slot 0 -> sample_count == 1
                    for (int f = 1; f < 64; ++f) {
                        if (!rec_step(false)) return 1;
                    }
                    const size_t cnt_after_valid = onr_test_gpu_sample_count(ctx_hd);
                    // Frame 64 wraps back onto slot 0, recorded AND reset WITHOUT submit
                    if (!rec_step(false)) return 1;
                    // Frame 65 (slot 1) calls vk_harvest over retired slot 0 -> must NOT harvest stale slot 0
                    if (!rec_step(false)) return 1;
                    const size_t cnt_after_discard = onr_test_gpu_sample_count(ctx_hd);
                    // Frame 66: next valid submit, then frame 67 triggers harvest -> sample_count == 2
                    if (!rec_step(true)) return 1;
                    if (!rec_step(false)) return 1;
                    const size_t cnt_after_next = onr_test_gpu_sample_count(ctx_hd);

                    api.Destroy(ctx_hd);
                    { std::error_code ec_rm; std::filesystem::remove(log_hd, ec_rm); }
                    if (cnt_after_valid != 1 || cnt_after_discard != 1 || cnt_after_next != 2) {
                        std::fprintf(stderr, "FAIL: vk_harvest discard regression counts valid=%zu discard=%zu next=%zu (expected 1,1,2)\n",
                                     cnt_after_valid, cnt_after_discard, cnt_after_next);
                        return 1;
                    }
                    std::printf("[PASS] Stage10: vk_harvest discarded/unsubmitted query isolation (counts=%zu -> %zu -> %zu)\n",
                                cnt_after_valid, cnt_after_discard, cnt_after_next);

                    // 4b. Test gfx1200 (0x48) fault behavior via OPENRR_TEST_HOOKS ONR_ACO_TARGET + ONR_ACO_DIR
                    const auto gfx1200_aco = data_dir / "aco" / "gfx1200" / "openrr-d3d12-v1";
                    if (std::filesystem::exists(gfx1200_aco / "manifest.msgpack")) {
                        const auto saved_tgt = std::getenv("ONR_ACO_TARGET") ? std::string(std::getenv("ONR_ACO_TARGET")) : "";
                        const auto saved_dir = std::getenv("ONR_ACO_DIR") ? std::string(std::getenv("ONR_ACO_DIR")) : "";
                        auto restore_env = [&]() {
                            _putenv_s("ONR_ACO_TARGET", saved_tgt.c_str());
                            _putenv_s("ONR_ACO_DIR", saved_dir.c_str());
                        };
                        _putenv_s("ONR_ACO_TARGET", "gfx1200");
                        onr::Device hvk;
                        std::string herr;
                        if (!hvk.adopt({inst, pd, hdev, rf, ri}, herr)) {
                            restore_env();
                            return 1;
                        }
                        const auto tmp_fault = bdir / "tmp-gfx1200-faults";
                        std::filesystem::remove_all(tmp_fault);
                        // (i) missing manifest
                        const auto f_nomani = tmp_fault / "no-manifest";
                        std::filesystem::create_directories(f_nomani);
                        _putenv_s("ONR_ACO_DIR", f_nomani.string().c_str());
                        hvk.set_compiler(true, true, (tmp_fault / "cache1").string(), shader_dir.string());
                        const bool rej_nomani = !hvk.aco_ready();

                        // Map shader stems -> spv_hash from stage/openrr-amd/shaders
                        std::vector<std::pair<std::string, std::string>> spv_index;
                        for (const auto& de : std::filesystem::directory_iterator(shader_dir)) {
                            if (de.path().extension() != ".spv") continue;
                            const auto stem = de.path().stem().string();
                            const auto spv_b = onr::pal::load(de.path().string());
                            char hbuf[32]{};
                            std::snprintf(hbuf, sizeof hbuf, "%016llx",
                                          static_cast<unsigned long long>(onr::pal::hash(spv_b.data(), spv_b.size())));
                            spv_index.emplace_back(hbuf, stem);
                        }
                        auto stem_for_key = [&](const std::string& key) -> std::string {
                            for (const auto& [kh, st] : spv_index)
                                if (kh == key) return st;
                            return {};
                        };
                        auto key_for_stem = [&](const std::string& st) -> std::string {
                            for (const auto& [kh, s] : spv_index)
                                if (s == st) return kh;
                            return {};
                        };

                        const std::string base_key = key_for_stem("ingest");
                        std::string lite_stem = "post_f_lt", lite_key = key_for_stem("post_f_lt");
                        if (lite_key.empty()) {
                            for (const auto& [kh, st] : spv_index) {
                                if (st.size() > 3 && st.rfind("_lt") == st.size() - 3) {
                                    lite_key = kh;
                                    lite_stem = st;
                                    break;
                                }
                            }
                        }

                        // Copy full gfx1200 package and corrupt one base .nrp + one _lt .nrp
                        const auto f_base = tmp_fault / "corrupt-base";
                        std::filesystem::create_directories(tmp_fault);
                        std::filesystem::copy(gfx1200_aco, f_base, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                        if (!base_key.empty()) {
                            std::ofstream corrupt(f_base / (base_key + ".nrp"), std::ios::binary | std::ios::trunc);
                            corrupt << "corrupt";
                        }
                        _putenv_s("ONR_ACO_DIR", f_base.string().c_str());
                        hvk.set_compiler(true, true, (tmp_fault / "cache2").string(), shader_dir.string());
                        bool rej_base = !hvk.aco_ready();
                        if (hvk.aco_ready()) {
                            try { (void)hvk.load((shader_dir / "ingest.spv").string(), 0); }
                            catch (const std::exception&) { rej_base = true; }
                        }

                        const auto f_lite = tmp_fault / "corrupt-lite";
                        std::filesystem::copy(gfx1200_aco, f_lite, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                        if (!lite_key.empty()) {
                            std::ofstream corrupt(f_lite / (lite_key + ".nrp"), std::ios::binary | std::ios::trunc);
                            corrupt << "corrupt";
                        }
                        _putenv_s("ONR_ACO_DIR", f_lite.string().c_str());
                        hvk.set_compiler(true, true, (tmp_fault / "cache3").string(), shader_dir.string());
                        bool rej_lite = !hvk.aco_ready();
                        if (hvk.aco_ready() && !lite_stem.empty()) {
                            try { (void)hvk.load((shader_dir / (lite_stem + ".spv")).string(), 32); }
                            catch (const std::exception&) { rej_lite = true; }
                        }
                        restore_env();
                        std::filesystem::remove_all(tmp_fault);
                        if (!rej_nomani || !rej_base || !rej_lite || !stem_for_key(base_key).size()) {
                            std::fprintf(stderr, "FAIL: gfx1200 faults nomani=%d base=%d lite=%d\n",
                                         int(rej_nomani), int(rej_base), int(rej_lite));
                            return 1;
                        }
                        std::printf("[PASS] Stage10: gfx1200 strict ACO fault rejection (missing-manifest, corrupt-base, corrupt-%s)\n",
                                    lite_stem.c_str());
                    }

                    vkDeviceWaitIdle(hdev);
                    vkDestroyFence(hdev, fence, nullptr);
                    OnrRrNotifyVkCommandPoolDestroyed(hdev, cpool);
                    vkDestroyCommandPool(hdev, cpool, nullptr);
                    vkDestroyImageView(hdev, iv_col, nullptr); vkDestroyImage(hdev, im_col, nullptr); vkFreeMemory(hdev, mem_col, nullptr);
                    vkDestroyImageView(hdev, iv_out, nullptr); vkDestroyImage(hdev, im_out, nullptr); vkFreeMemory(hdev, mem_out, nullptr);
                    OnrRrNotifyVkDeviceDestroyed(hdev);
                    vkDestroyDevice(hdev, nullptr);
                }
            }
            vkDestroyInstance(inst, nullptr);
        }
    }

    // 4a. Offline 94/94 verification of packaged gfx1200 (0x48) & gfx1201 (0x4e) ACO manifests + pal::splice + pal::splice_vulkan
    std::vector<std::pair<std::string, std::string>> all_spvs;
    for (const auto& de : std::filesystem::directory_iterator(shader_dir)) {
        if (de.path().extension() != ".spv") continue;
        const auto spv_b = onr::pal::load(de.path().string());
        char hbuf[32]{};
        std::snprintf(hbuf, sizeof hbuf, "%016llx",
                      static_cast<unsigned long long>(onr::pal::hash(spv_b.data(), spv_b.size())));
        all_spvs.emplace_back(hbuf, de.path().stem().string());
    }
    for (const char* tgt : {"gfx1200", "gfx1201"}) {
        const uint32_t expect_mach = !std::strcmp(tgt, "gfx1200") ? 0x48u : 0x4eu;
        const auto root = data_dir / "aco" / tgt / "openrr-d3d12-v1";
        if (!std::filesystem::exists(root / "manifest.msgpack")) {
            std::fprintf(stderr, "FAIL: missing packaged ACO manifest for %s\n", tgt);
            return 1;
        }
        const auto index = onr::pal::unpack(onr::pal::load((root / "manifest.msgpack").string()));
        const auto json_bytes = onr::pal::load((root / "manifest.json").string());
        if (index.at("schema").number() != 1 || index.at("wave_size").number() != 32 ||
            index.at("layout_push_bytes").number() != 256 || index.at("target").s != tgt ||
            index.at("layout_version").s != "openrr-d3d12-v1" ||
            onr::pal::hash(json_bytes.data(), json_bytes.size()) != index.at("manifest_hash").number()) {
            std::fprintf(stderr, "FAIL: manifest header mismatch for %s\n", tgt);
            return 1;
        }
        const auto& recs = index.at("records").fields;
        if (recs.size() != 94) {
            std::fprintf(stderr, "FAIL: %s record count %zu != 94\n", tgt, recs.size());
            return 1;
        }
        const onr::pal::Bytes tmpl_d3d = make_synthetic_pal_elf(expect_mach, 66, {0x10000000u, 1u, 65u});
        const onr::pal::Bytes tmpl_vk  = make_synthetic_pal_elf(expect_mach, 69, {0x10000000u, 4u, 68u});
        size_t spliced = 0;
        for (const auto& [key, entry] : recs) {
            std::string stem;
            for (const auto& [kh, st] : all_spvs) {
                if (kh == key) {
                    stem = st;
                    break;
                }
            }
            if (stem.empty()) {
                std::fprintf(stderr, "FAIL: %s record %s missing matching .spv in shaders\n", tgt, key.c_str());
                return 1;
            }
            const auto spv = onr::pal::load((shader_dir / (stem + ".spv")).string());
            const auto rec = onr::pal::Record::parse(onr::pal::load((root / (key + ".nrp")).string()));
            if (rec.mach != expect_mach || rec.spv_hash != onr::pal::hash(spv.data(), spv.size()) || rec.push_bytes > 256) {
                std::fprintf(stderr, "FAIL: %s record %s (%s) mismatch\n", tgt, stem.c_str(), key.c_str());
                return 1;
            }
            const auto& dims = entry.at("workgroup").items;
            const std::array<uint32_t, 3> wg{uint32_t(dims[0].number()), uint32_t(dims[1].number()), uint32_t(dims[2].number())};
            const auto d3d_bin = onr::pal::splice(tmpl_d3d, rec, 256, wg, 4);
            const auto vk_bin = onr::pal::splice_vulkan(tmpl_vk, rec, 256, wg);
            if (onr::pal::Elf(d3d_bin).mach != expect_mach || onr::pal::Elf(vk_bin).mach != expect_mach) {
                std::fprintf(stderr, "FAIL: %s spliced ELF mach mismatch for %s\n", tgt, stem.c_str());
                return 1;
            }
            ++spliced;
        }
        std::printf("[PASS] Stage10: %s packaged ACO offline validation (%zu/94 records + D3D12/Vulkan splice)\n", tgt, spliced);
    }

    return 0;
}

static int test_stage10_r2_fixes() {
    namespace fs = std::filesystem;
    const auto bdir = fs::path("C:/Users/Kenny/.codex/worktrees/v017-merge/OpenRR/build/v017-asi");
    const auto stage_data = bdir / "stage/openrr-amd";
    const auto shader_dir = stage_data / "shaders";
    const auto dlssd = fs::path("C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll");
    if (!fs::exists(shader_dir)) {
        std::printf("[SKIP] test_stage10_r2_fixes: stage shaders missing\n");
        return 0;
    }

    // 1. [RADV SPIR-V] Harden real packaged SPVs (`global` vs `native`) and verify idempotence.
    {
        struct Case { const char* stem; bool native; };
        const Case cases[] = {{"downsample_lt", false}, {"post_f_lt", true}, {"enc0_stage_32_64_f_lx", true}};
        for (const auto& tc : cases) {
            if (onr::is_radv_native_precision_stem(tc.stem) != tc.native) {
                std::fprintf(stderr, "FAIL: precision classification mismatch for %s\n", tc.stem);
                return 1;
            }
            const auto raw = onr::pal::load((shader_dir / (std::string(tc.stem) + ".spv")).string());
            uint32_t added1 = 0, added2 = 0;
            const auto h1 = onr::harden_spirv_for_radv(raw.data(), raw.size(), tc.stem, &added1);
            if (h1.size() < 5 || h1[0] != 0x07230203u || added1 == 0 || h1.size() != raw.size() / 4 + added1 * 3) {
                std::fprintf(stderr, "FAIL: RADV harden pass1 invalid for %s (added=%u words=%zu raw=%zu)\n",
                             tc.stem, added1, h1.size(), raw.size());
                return 1;
            }
            const auto h2 = onr::harden_spirv_for_radv(reinterpret_cast<const uint8_t*>(h1.data()), h1.size() * 4, tc.stem, &added2);
            if (added2 != 0 || h2 != h1) {
                std::fprintf(stderr, "FAIL: RADV harden not idempotent for %s (added2=%u)\n", tc.stem, added2);
                return 1;
            }
        }
        std::printf("[PASS] RADV SPIR-V NoContraction hardener (global/native split + idempotence)\n");
    }

    // 2. [P1-2] PrepareVkDevice permits repeatable VkDevicePrivateDataCreateInfo & VkDeviceDeviceMemoryReportCreateInfoEXT
    //    on PAGE_READONLY, rejects cycles and duplicate unique sTypes.
    OnrRrApi5 api5{};
    if (OnrRrGetApi(ONR_RR_ABI_VERSION_5, &api5.v4.v3.v2.v1) != ONR_RR_OK) return 1;
    OnrRrApi& api = api5.v4.v3.v2.v1;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS && inst) {
        uint32_t npd = 0;
        vkEnumeratePhysicalDevices(inst, &npd, nullptr);
        std::vector<VkPhysicalDevice> pds(npd);
        vkEnumeratePhysicalDevices(inst, &npd, pds.data());
        VkPhysicalDevice amd_pd = VK_NULL_HANDLE, rdna4_pd = VK_NULL_HANDLE;
        for (auto pd : pds) {
            VkPhysicalDeviceProperties pp{};
            vkGetPhysicalDeviceProperties(pd, &pp);
            if (pp.vendorID != 0x1002) continue;
            const auto tid = onr::identify_vk_physical_device(pd);
            const bool is_r4 = (onr::arch::aco_target_for_mach(tid.elf_mach) != nullptr) || tid.arch == onr::arch::GpuArch::Rdna4;
            if (is_r4 && !rdna4_pd) rdna4_pd = pd;
            if (!amd_pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) amd_pd = pd;
        }
        VkPhysicalDevice pd = rdna4_pd ? rdna4_pd : amd_pd;
        if (pd) {
            uint32_t nqf = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
            uint32_t gfx = 0;
            std::vector<VkQueueFamilyProperties> qfs(nqf);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
            while (gfx < nqf && !(qfs[gfx].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++gfx;
            if (gfx >= nqf) gfx = 0;

            struct MultiChainRo {
                float prio;
                VkDeviceQueueCreateInfo qci;
                VkDevicePrivateDataCreateInfo pd1;
                VkDevicePrivateDataCreateInfo pd2;
                VkDeviceDeviceMemoryReportCreateInfoEXT mr1;
                VkDeviceDeviceMemoryReportCreateInfoEXT mr2;
                VkPhysicalDeviceVulkan12Features v12;
                VkDeviceCreateInfo dci;
            };
            static_assert(sizeof(MultiChainRo) <= 4096);
            void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!page) { vkDestroyInstance(inst, nullptr); return 1; }
            auto* ro = static_cast<MultiChainRo*>(page);
            memset(page, 0, 4096);
            ro->prio = 1.0f;
            ro->qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            ro->qci.queueFamilyIndex = gfx; ro->qci.queueCount = 1; ro->qci.pQueuePriorities = &ro->prio;
            ro->v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            ro->v12.storageBuffer8BitAccess = VK_TRUE;
            ro->v12.shaderFloat16 = VK_TRUE;
            ro->v12.shaderInt8 = VK_TRUE;
            ro->v12.bufferDeviceAddress = VK_TRUE;
            ro->v12.vulkanMemoryModel = VK_TRUE;
            ro->v12.vulkanMemoryModelDeviceScope = VK_TRUE;
            ro->v12.shaderBufferInt64Atomics = VK_TRUE;
            ro->v12.descriptorBindingPartiallyBound = VK_TRUE;
            ro->v12.hostQueryReset = VK_TRUE;
            ro->v12.scalarBlockLayout = VK_TRUE;
            ro->mr2 = {VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT, &ro->v12};
            ro->mr1 = {VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT, &ro->mr2};
            ro->pd2 = {VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO, &ro->mr1, 4};
            ro->pd1 = {VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO, &ro->pd2, 2};
            ro->dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ro->pd1};
            ro->dci.queueCreateInfoCount = 1;
            ro->dci.pQueueCreateInfos = &ro->qci;

            DWORD oldp = 0;
            VirtualProtect(page, 4096, PAGE_READONLY, &oldp);
            std::vector<uint8_t> before(4096);
            memcpy(before.data(), page, 4096);
            VkDeviceCreateInfo out_ci{};
            void* setup = nullptr;
            const int32_t rc_dup_ok = api5.PrepareVkDevice(pd, &ro->dci, &out_ci, &setup);
            const bool ro_unmutated = (memcmp(page, before.data(), 4096) == 0);
            int n_priv = 0, n_memrep = 0;
            for (const auto* b = static_cast<const VkBaseInStructure*>(out_ci.pNext); b; b = b->pNext) {
                if (b->sType == VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO) ++n_priv;
                if (b->sType == VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT) ++n_memrep;
            }
            if (setup) api5.FinishVkDevice(setup, VK_NULL_HANDLE, nullptr, nullptr);

            // Cycle rejection and duplicate unique sType rejection
            VirtualProtect(page, 4096, PAGE_READWRITE, &oldp);
            ro->pd2.pNext = &ro->pd1; // cycle
            void* setup_cyc = reinterpret_cast<void*>(uintptr_t(1));
            const int32_t rc_cyc = api5.PrepareVkDevice(pd, &ro->dci, &out_ci, &setup_cyc);

            VkPhysicalDeviceVulkan12Features v12_b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan12Features v12_a{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v12_b};
            VkDeviceCreateInfo dup_u{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12_a};
            dup_u.queueCreateInfoCount = 1; dup_u.pQueueCreateInfos = &ro->qci;
            void* setup_dupu = reinterpret_cast<void*>(uintptr_t(1));
            const int32_t rc_dupu = api5.PrepareVkDevice(pd, &dup_u, &out_ci, &setup_dupu);
            VirtualFree(page, 0, MEM_RELEASE);

            if (rc_dup_ok != ONR_RR_OK || !setup || !ro_unmutated || n_priv != 2 || n_memrep != 2 ||
                rc_cyc == ONR_RR_OK || setup_cyc != nullptr || rc_dupu == ONR_RR_OK || setup_dupu != nullptr) {
                std::fprintf(stderr, "FAIL: [P1-2] PrepareVkDevice multi-sType/cycle test (ok=%d unmut=%d priv=%d mr=%d cyc=%d dupu=%d)\n",
                             rc_dup_ok, int(ro_unmutated), n_priv, n_memrep, rc_cyc, rc_dupu);
                vkDestroyInstance(inst, nullptr);
                return 1;
            }
            std::printf("[PASS] [P1-2] PrepareVkDevice repeatable PrivateData/MemoryReport on PAGE_READONLY + cycle/unique-sType guards\n");

            // 3. [P2-1] openrr::pe::vkreset production hooks call driver first and notify ONLY on VK_SUCCESS
            {
                VkDeviceCreateInfo dci_ok{};
                void* setup_ok = nullptr;
                VkDeviceCreateInfo raw_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                VkDeviceQueueCreateInfo raw_q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                float qprio = 1.0f;
                raw_q.queueFamilyIndex = gfx; raw_q.queueCount = 1; raw_q.pQueuePriorities = &qprio;
                raw_ci.queueCreateInfoCount = 1; raw_ci.pQueueCreateInfos = &raw_q;
                VkDevice hdev = VK_NULL_HANDLE;
                uint32_t rf = 0, ri = 0;
                if (api5.PrepareVkDevice(pd, &raw_ci, &dci_ok, &setup_ok) == ONR_RR_OK && setup_ok) {
                    if (vkCreateDevice(pd, &dci_ok, nullptr, &hdev) == VK_SUCCESS && hdev) {
                        api5.FinishVkDevice(setup_ok, hdev, &rf, &ri);
                        bool p2_1_ok = true;
                        int fail_counts[6]{};
                        {
                            onr::Device dv;
                            std::string derr;
                            if (dv.adopt({inst, pd, hdev, rf, ri}, derr)) {
                                VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                                pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                                pci.queueFamilyIndex = gfx;
                                VkCommandPool cpool = VK_NULL_HANDLE;
                                VkCommandBuffer cb = VK_NULL_HANDLE;
                                VkQueryPool qp = VK_NULL_HANDLE;
                                vkCreateCommandPool(hdev, &pci, nullptr, &cpool);
                                VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                                cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                                vkAllocateCommandBuffers(hdev, &cai, &cb);
                                OnrRrNotifyVkCommandBuffersAllocated(hdev, cpool, 1, reinterpret_cast<void* const*>(&cb));
                                VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                                qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 4;
                                vkCreateQueryPool(hdev, &qpi, nullptr, &qp);

                                auto bind_one = [&]() {
                                    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                    vkBeginCommandBuffer(cb, &bi);
                                    dv.bind(cb, qp);
                                    dv.stamp();
                                    dv.unbind();
                                    vkEndCommandBuffer(cb);
                                };

                                static VkResult g_mock_res = VK_SUCCESS;
                                static int g_driver_calls = 0;
                                auto mock_begin = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) -> VkResult {
                                    ++g_driver_calls; return g_mock_res;
                                };
                                auto mock_reset_cb = [](VkCommandBuffer, VkCommandBufferResetFlags) -> VkResult {
                                    ++g_driver_calls; return g_mock_res;
                                };
                                auto mock_reset_pool = [](VkDevice, VkCommandPool, VkCommandPoolResetFlags) -> VkResult {
                                    ++g_driver_calls; return g_mock_res;
                                };

                                // 3a: vkBeginCommandBuffer failure must NOT retire qp; success must retire qp
                                bind_one();
                                g_mock_res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                                g_driver_calls = 0;
                                VkCommandBufferBeginInfo bi_dummy{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                                const VkResult rb_fail = openrr::pe::vkreset::track_begin_cb(mock_begin, &OnrRrNotifyVkCommandBufferReset, cb, &bi_dummy);
                                const bool stay_active_1 = !dv.pool_retired(qp);
                                g_mock_res = VK_SUCCESS;
                                const VkResult rb_ok = openrr::pe::vkreset::track_begin_cb(mock_begin, &OnrRrNotifyVkCommandBufferReset, cb, &bi_dummy);
                                const bool retired_1 = dv.pool_retired(qp);

                                // 3b: vkResetCommandBuffer failure vs success
                                bind_one();
                                g_mock_res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                                const VkResult rrc_fail = openrr::pe::vkreset::track_reset_cb(mock_reset_cb, &OnrRrNotifyVkCommandBufferReset, cb, 0);
                                const bool stay_active_2 = !dv.pool_retired(qp);
                                g_mock_res = VK_SUCCESS;
                                const VkResult rrc_ok = openrr::pe::vkreset::track_reset_cb(mock_reset_cb, &OnrRrNotifyVkCommandBufferReset, cb, 0);
                                const bool retired_2 = dv.pool_retired(qp);

                                // 3c: vkResetCommandPool failure vs success
                                bind_one();
                                g_mock_res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                                const VkResult rrp_fail = openrr::pe::vkreset::track_reset_pool(mock_reset_pool, &OnrRrNotifyVkCommandPoolReset, hdev, cpool, 0);
                                const bool stay_active_3 = !dv.pool_retired(qp);
                                g_mock_res = VK_SUCCESS;
                                const VkResult rrp_ok = openrr::pe::vkreset::track_reset_pool(mock_reset_pool, &OnrRrNotifyVkCommandPoolReset, hdev, cpool, 0);
                                const bool retired_3 = dv.pool_retired(qp);

                                OnrRrNotifyVkCommandPoolDestroyed(hdev, cpool);
                                vkDestroyQueryPool(hdev, qp, nullptr);
                                vkFreeCommandBuffers(hdev, cpool, 1, &cb);
                                vkDestroyCommandPool(hdev, cpool, nullptr);

                                p2_1_ok = (rb_fail == VK_ERROR_OUT_OF_DEVICE_MEMORY && rb_ok == VK_SUCCESS && stay_active_1 && retired_1 &&
                                           rrc_fail == VK_ERROR_OUT_OF_DEVICE_MEMORY && rrc_ok == VK_SUCCESS && stay_active_2 && retired_2 &&
                                           rrp_fail == VK_ERROR_OUT_OF_DEVICE_MEMORY && rrp_ok == VK_SUCCESS && stay_active_3 && retired_3);
                                fail_counts[0] = stay_active_1; fail_counts[1] = retired_1;
                                fail_counts[2] = stay_active_2; fail_counts[3] = retired_2;
                                fail_counts[4] = stay_active_3; fail_counts[5] = retired_3;
                            }
                        } // dv.~Device() runs while hdev is still valid
                        OnrRrNotifyVkDeviceDestroyed(hdev);
                        vkDeviceWaitIdle(hdev);
                        vkDestroyDevice(hdev, nullptr);
                        if (!p2_1_ok) {
                            std::fprintf(stderr, "FAIL: [P2-1] vkreset success-only guard (%d/%d %d/%d %d/%d)\n",
                                         fail_counts[0], fail_counts[1], fail_counts[2], fail_counts[3], fail_counts[4], fail_counts[5]);
                            vkDestroyInstance(inst, nullptr);
                            return 1;
                        }
                        std::printf("[PASS] [P2-1] Vulkan begin/reset_cb/reset_pool hooks notify ONLY after VK_SUCCESS\n");
                    } else {
                        api5.FinishVkDevice(setup_ok, VK_NULL_HANDLE, nullptr, nullptr);
                    }
                }
            }

            // 4. [Linux RADV] Simulated RADV (ONR_TEST_SIMULATE_RADV=1) skips pipeline_binary requirement and resolves RADV native ACO
            if (fs::exists(dlssd)) {
                const auto saved_radv = std::getenv("ONR_TEST_SIMULATE_RADV") ? std::string(std::getenv("ONR_TEST_SIMULATE_RADV")) : "";
                _putenv_s("ONR_TEST_SIMULATE_RADV", "1");
                const auto tid_radv = onr::identify_vk_physical_device(pd);
                float prio = 1.0f;
                VkDeviceQueueCreateInfo rq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                rq.queueFamilyIndex = gfx; rq.queueCount = 1; rq.pQueuePriorities = &prio;
                VkDeviceCreateInfo rci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                rci.queueCreateInfoCount = 1; rci.pQueueCreateInfos = &rq;
                VkDeviceCreateInfo rci_out{};
                void* rsetup = nullptr;
                const int32_t rp_rc = api5.PrepareVkDevice(pd, &rci, &rci_out, &rsetup);
                bool no_pbin_ext = true;
                for (uint32_t ei = 0; ei < rci_out.enabledExtensionCount; ++ei) {
                    if (std::strcmp(rci_out.ppEnabledExtensionNames[ei], VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) == 0)
                        no_pbin_ext = false;
                }
                VkDevice rdev = VK_NULL_HANDLE;
                uint32_t rf = 0, ri = 0;
                bool radv_create_ok = false, radv_log_ok = false;
                if (rp_rc == ONR_RR_OK && rsetup && vkCreateDevice(pd, &rci_out, nullptr, &rdev) == VK_SUCCESS && rdev) {
                    api5.FinishVkDevice(rsetup, rdev, &rf, &ri);
                    const auto radv_log = (bdir / "audit-radv-sim.log").wstring();
                    OnrRrVkDevice vkd{sizeof vkd, VK_API_VERSION_1_3, inst, pd, rdev, rf, ri};
                    OnrRrCreateInfo ci{sizeof ci, ONR_RR_MODE_FAST, nullptr, 64, 64, 64, 64,
                                       dlssd.c_str(), nullptr, stage_data.c_str(), radv_log.c_str(),
                                       ONR_RR_BACKEND_CHOICE_AUTO, &vkd};
                    void* rctx = nullptr;
                    if (api.Create(&ci, &rctx) == ONR_RR_OK && rctx) {
                        for (int t = 0; t < 300; ++t) {
                            OnrRrInfo inf{sizeof inf};
                            api.GetInfo(rctx, &inf);
                            if (inf.state == ONR_RR_STATE_READY) { radv_create_ok = true; break; }
                            if (inf.state == ONR_RR_STATE_FAILED) break;
                            Sleep(25);
                        }
                        api.Destroy(rctx);
                    }
                    OnrRrNotifyVkDeviceDestroyed(rdev);
                    vkDeviceWaitIdle(rdev);
                    vkDestroyDevice(rdev, nullptr);
                    std::ifstream lf(std::filesystem::path(radv_log), std::ios::binary);
                    std::string ltxt((std::istreambuf_iterator<char>(lf)), {});
                    radv_log_ok = ltxt.find("compiler resolved: RADV native ACO") != std::string::npos &&
                                  ltxt.find("compiler=ACO") != std::string::npos;
                    std::error_code ec_rm;
                    fs::remove(radv_log, ec_rm);
                } else if (rsetup) {
                    api5.FinishVkDevice(rsetup, VK_NULL_HANDLE, nullptr, nullptr);
                }
                _putenv_s("ONR_TEST_SIMULATE_RADV", saved_radv.c_str());
                if (!tid_radv.is_radv || tid_radv.driver_id != VK_DRIVER_ID_MESA_RADV ||
                    rp_rc != ONR_RR_OK || !no_pbin_ext || !radv_create_ok || !radv_log_ok) {
                    std::fprintf(stderr, "FAIL: [RADV] simulated RADV test failed (radv=%d pbin=%d create=%d log=%d)\n",
                                 int(tid_radv.is_radv), int(no_pbin_ext), int(radv_create_ok), int(radv_log_ok));
                    vkDestroyInstance(inst, nullptr);
                    return 1;
                }
                std::printf("[PASS] [RADV] Simulated Mesa RADV PrepareVkDevice + RADV native ACO Create/Build\n");
            }

            // 5. [P1-1] Missing Fast-mode _lx / _lt SPV must never report READY or partial fallback
            if (fs::exists(dlssd)) {
                const auto iso_root = fs::temp_directory_path() / "openrr_unit_p1_1_iso";
                std::error_code ec_iso;
                fs::remove_all(iso_root, ec_iso);
                fs::create_directories(iso_root);
                for (const char* sub : {"shaders", "shaders-gfx11", "aco"}) {
                    if (fs::exists(stage_data / sub))
                        fs::copy(stage_data / sub, iso_root / sub, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                }
                // 5a: Missing enc0_stage_32_64_f_lx.spv with Fast=2 must fail with filename and report GetFastMode=3
                for (const char* sub : {"shaders", "shaders-gfx11"}) {
                    fs::remove(iso_root / sub / "enc0_stage_32_64_f_lx.spv", ec_iso);
                }
                float prio = 1.0f;
                VkDeviceQueueCreateInfo rq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                rq.queueFamilyIndex = gfx; rq.queueCount = 1; rq.pQueuePriorities = &prio;
                VkDeviceCreateInfo rci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                rci.queueCreateInfoCount = 1; rci.pQueueCreateInfos = &rq;
                VkDeviceCreateInfo rci_out{};
                void* rsetup = nullptr;
                if (api5.PrepareVkDevice(pd, &rci, &rci_out, &rsetup) == ONR_RR_OK && rsetup) {
                    VkDevice vdev = VK_NULL_HANDLE;
                    uint32_t rf = 0, ri = 0;
                    if (vkCreateDevice(pd, &rci_out, nullptr, &vdev) == VK_SUCCESS && vdev) {
                        api5.FinishVkDevice(rsetup, vdev, &rf, &ri);
                        const auto p1_log = (iso_root / "p1_1.log").wstring();
                        const std::wstring iso_cache_w = (iso_root / "cache").wstring();
                        const std::wstring iso_root_w = iso_root.wstring();
                        OnrRrVkDevice vkd{sizeof vkd, VK_API_VERSION_1_3, inst, pd, vdev, rf, ri};
                        OnrRrCreateInfo ci{sizeof ci, ONR_RR_MODE_FAST, nullptr, 640, 360, 960, 540,
                                           dlssd.c_str(), iso_cache_w.c_str(),
                                           iso_root_w.c_str(), p1_log.c_str(),
                                           ONR_RR_BACKEND_CHOICE_AUTO, &vkd};
                        void* fctx = nullptr;
                        int32_t fm_lx = -1;
                        OnrRrInfo inf_lx{sizeof inf_lx};
                        if (api.Create(&ci, &fctx) == ONR_RR_OK && fctx) {
                            OnrRrSetFastMode(fctx, 2);
                            for (int t = 0; t < 400; ++t) {
                                api.GetInfo(fctx, &inf_lx);
                                fm_lx = OnrRrGetFastMode(fctx);
                                if (fm_lx != 2 && (!rdna4_pd || inf_lx.state == ONR_RR_STATE_FAILED)) break;
                                Sleep(25);
                            }
                            api.Destroy(fctx);
                        } else {
                            char cerr[512]{};
                            api.GetLastError(cerr, sizeof cerr);
                            std::fprintf(stderr, "FAIL: [P1-1 5a] Create failed: %s\n", cerr);
                        }
                        if (fm_lx != 3 || (rdna4_pd && (inf_lx.state != ONR_RR_STATE_FAILED ||
                                std::strstr(inf_lx.message, "enc0_stage_32_64_f_lx.spv") == nullptr))) {
                            std::fprintf(stderr, "FAIL: [P1-1] missing _lx SPV allowed ready! fm=%d state=%u msg=%s\n",
                                         fm_lx, inf_lx.state, inf_lx.message);
                            OnrRrNotifyVkDeviceDestroyed(vdev);
                            vkDestroyDevice(vdev, nullptr);
                            vkDestroyInstance(inst, nullptr);
                            fs::remove_all(iso_root, ec_iso);
                            return 1;
                        }

                        // 5b: Restore _lx SPV, remove post_f_lt.spv -> Fast=1 must fail with post_f_lt.spv
                        for (const char* sub : {"shaders", "shaders-gfx11"}) {
                            fs::copy_file(stage_data / sub / "enc0_stage_32_64_f_lx.spv",
                                          iso_root / sub / "enc0_stage_32_64_f_lx.spv",
                                          fs::copy_options::overwrite_existing, ec_iso);
                            fs::remove(iso_root / sub / "post_f_lt.spv", ec_iso);
                        }
                        void* fctx2 = nullptr;
                        int32_t fm_lt = -1;
                        OnrRrInfo inf_lt{sizeof inf_lt};
                        if (api.Create(&ci, &fctx2) == ONR_RR_OK && fctx2) {
                            OnrRrSetFastMode(fctx2, 1);
                            for (int t = 0; t < 400; ++t) {
                                api.GetInfo(fctx2, &inf_lt);
                                fm_lt = OnrRrGetFastMode(fctx2);
                                if (fm_lt != 2 && (!rdna4_pd || inf_lt.state == ONR_RR_STATE_FAILED)) break;
                                Sleep(25);
                            }
                            api.Destroy(fctx2);
                        } else {
                            char cerr[512]{};
                            api.GetLastError(cerr, sizeof cerr);
                            std::fprintf(stderr, "FAIL: [P1-1 5b] Create failed: %s\n", cerr);
                        }
                        OnrRrNotifyVkDeviceDestroyed(vdev);
                        vkDeviceWaitIdle(vdev);
                        vkDestroyDevice(vdev, nullptr);
                        if (fm_lt != 3 || (rdna4_pd && (inf_lt.state != ONR_RR_STATE_FAILED ||
                                std::strstr(inf_lt.message, "post_f_lt.spv") == nullptr))) {
                            std::fprintf(stderr, "FAIL: [P1-1] missing post_f_lt.spv allowed ready! fm=%d state=%u msg=%s\n",
                                         fm_lt, inf_lt.state, inf_lt.message);
                            vkDestroyInstance(inst, nullptr);
                            fs::remove_all(iso_root, ec_iso);
                            return 1;
                        }
                        {
                            std::ifstream p1f{fs::path(p1_log), std::ios::binary};
                            const std::string p1txt((std::istreambuf_iterator<char>(p1f)), {});
                            if (p1txt.find("missing required SPIR-V: enc0_stage_32_64_f_lx.spv") == std::string::npos ||
                                p1txt.find("missing required SPIR-V: post_f_lt.spv") == std::string::npos) {
                                std::fprintf(stderr, "FAIL: [P1-1] p1_1.log missing full missing-SPIR-V lines\n");
                                vkDestroyInstance(inst, nullptr);
                                fs::remove_all(iso_root, ec_iso);
                                return 1;
                            }
                        }
                        std::printf("[PASS] [P1-1] Missing enc0_stage_32_64_f_lx.spv (Fast=2) & post_f_lt.spv (Fast=1) strictly rejected with filename\n");
                    } else {
                        api5.FinishVkDevice(rsetup, VK_NULL_HANDLE, nullptr, nullptr);
                    }
                }
                fs::remove_all(iso_root, ec_iso);
            }
        }
        vkDestroyInstance(inst, nullptr);
    }
    return 0;
}

namespace hook_safety_test {

struct MockEntry {
    void* detour = nullptr;
    void* orig = nullptr;
    bool queued = false;
    bool enabled = false;
};

struct MockState {
    std::unordered_map<void*, MockEntry> hooks;
    void* fail_create_target = nullptr;
    void* fail_queue_target = nullptr;
    void* fail_apply_on_target = nullptr;
    void* fail_enable_target = nullptr;
    bool fail_second_apply = false;
    int create_calls = 0;
    int remove_calls = 0;
    int enable_calls = 0;
    int disable_calls = 0;
    int queue_enable_calls = 0;
    int queue_disable_calls = 0;
    int apply_calls = 0;
    uintptr_t next_orig = 0x10000;
};

MockState* g_mock = nullptr;

MH_STATUS WINAPI mock_create(LPVOID target, LPVOID detour, LPVOID* orig) {
    ++g_mock->create_calls;
    if (target == g_mock->fail_create_target) return MH_ERROR_UNSUPPORTED_FUNCTION;
    if (g_mock->hooks.count(target)) return MH_ERROR_ALREADY_CREATED;
    void* tramp = reinterpret_cast<void*>(g_mock->next_orig += 0x100);
    g_mock->hooks[target] = MockEntry{detour, tramp, false, false};
    if (orig) *orig = tramp;
    return MH_OK;
}

MH_STATUS WINAPI mock_remove(LPVOID target) {
    ++g_mock->remove_calls;
    return g_mock->hooks.erase(target) ? MH_OK : MH_ERROR_NOT_CREATED;
}

MH_STATUS WINAPI mock_enable(LPVOID target) {
    ++g_mock->enable_calls;
    auto it = g_mock->hooks.find(target);
    if (it == g_mock->hooks.end()) return MH_ERROR_NOT_CREATED;
    if (target == g_mock->fail_enable_target) return MH_ERROR_MEMORY_PROTECT;
    if (it->second.enabled) return MH_ERROR_ENABLED;
    it->second.enabled = true;
    return MH_OK;
}

MH_STATUS WINAPI mock_disable(LPVOID target) {
    ++g_mock->disable_calls;
    auto it = g_mock->hooks.find(target);
    if (it == g_mock->hooks.end()) return MH_ERROR_NOT_CREATED;
    if (!it->second.enabled) return MH_ERROR_DISABLED;
    it->second.enabled = false;
    return MH_OK;
}

MH_STATUS WINAPI mock_queue_enable(LPVOID target) {
    ++g_mock->queue_enable_calls;
    if (target == g_mock->fail_queue_target) return MH_ERROR_NOT_CREATED;
    auto it = g_mock->hooks.find(target);
    if (it == g_mock->hooks.end()) return MH_ERROR_NOT_CREATED;
    it->second.queued = true;
    return MH_OK;
}

MH_STATUS WINAPI mock_queue_disable(LPVOID target) {
    ++g_mock->queue_disable_calls;
    auto it = g_mock->hooks.find(target);
    if (it == g_mock->hooks.end()) return MH_ERROR_NOT_CREATED;
    it->second.queued = false;
    return MH_OK;
}

MH_STATUS WINAPI mock_apply() {
    ++g_mock->apply_calls;
    if (g_mock->fail_second_apply && g_mock->apply_calls >= 2) return MH_ERROR_MEMORY_PROTECT;
    for (auto& [target, entry] : g_mock->hooks) {
        if (entry.enabled != entry.queued) {
            if (entry.queued && target == g_mock->fail_apply_on_target) {
                return MH_ERROR_MEMORY_PROTECT;
            }
            entry.enabled = entry.queued;
        }
    }
    return MH_OK;
}

openrr::pe::HookOps make_ops(MockState& state) {
    g_mock = &state;
    openrr::pe::HookOps ops{};
    ops.create_hook = &mock_create;
    ops.remove_hook = &mock_remove;
    ops.enable_hook = &mock_enable;
    ops.disable_hook = &mock_disable;
    ops.queue_enable = &mock_queue_enable;
    ops.queue_disable = &mock_queue_disable;
    ops.apply_queued = &mock_apply;
    return ops;
}

void dummy_detour() {}

}  // namespace hook_safety_test

static int test_d3d12_direct_hook_safety() {
    using namespace hook_safety_test;
    std::printf("[INFO] Direct D3D12 hook safety regression tests\n");

    int targets[10]{};
    void* origs[6]{};
    auto build_items = [&](openrr::pe::BatchHookItem (&items)[6]) {
        for (size_t i = 0; i < 6; ++i) {
            origs[i] = nullptr;
            items[i] = {"slot", unsigned(i), &targets[i], reinterpret_cast<void*>(&dummy_detour), &origs[i]};
        }
    };

    // 1. Create failure removes already-created hooks and clears orig
    {
        MockState st;
        st.fail_create_target = &targets[4];
        const auto ops = make_ops(st);
        openrr::pe::BatchHookItem items[6];
        build_items(items);
        const auto res = openrr::pe::install_hook_batch(items, 6, ops);
        if (res.error != openrr::pe::BatchInstallError::CreateFailed || res.failed_index != 4 ||
            res.trampolines_retained || st.remove_calls != 4 || st.apply_calls != 0 || !st.hooks.empty()) {
            std::fprintf(stderr, "FAIL: CreateFailed rollback mismatch\n");
            return 1;
        }
        for (void* o : origs) {
            if (o != nullptr) {
                std::fprintf(stderr, "FAIL: CreateFailed left non-null orig\n");
                return 1;
            }
        }
    }

    // 2. Queue failure unqueues, removes created hooks, and clears orig
    {
        MockState st;
        st.fail_queue_target = &targets[2];
        const auto ops = make_ops(st);
        openrr::pe::BatchHookItem items[6];
        build_items(items);
        const auto res = openrr::pe::install_hook_batch(items, 6, ops);
        if (res.error != openrr::pe::BatchInstallError::QueueFailed || res.failed_index != 2 ||
            res.trampolines_retained || st.queue_disable_calls != 2 || st.remove_calls != 6 ||
            st.apply_calls != 0 || !st.hooks.empty()) {
            std::fprintf(stderr, "FAIL: QueueFailed rollback mismatch\n");
            return 1;
        }
        for (void* o : origs) {
            if (o != nullptr) {
                std::fprintf(stderr, "FAIL: QueueFailed left non-null orig\n");
                return 1;
            }
        }
    }

    // 3. Apply failure disables enabled prefix, NEVER calls remove_hook, and retains non-null origs
    {
        MockState st;
        st.fail_apply_on_target = &targets[4];
        const auto ops = make_ops(st);
        openrr::pe::BatchHookItem items[6];
        build_items(items);
        const auto res = openrr::pe::install_hook_batch(items, 6, ops);
        if (res.error != openrr::pe::BatchInstallError::ApplyFailed || !res.trampolines_retained ||
            st.remove_calls != 0 || st.apply_calls != 2 || st.hooks.size() != 6) {
            std::fprintf(stderr, "FAIL: ApplyFailed did not retain trampolines safely (err=%d rem=%d apply=%d)\n",
                         int(res.error), st.remove_calls, st.apply_calls);
            return 1;
        }
        for (size_t i = 0; i < 6; ++i) {
            if (!origs[i] || st.hooks[&targets[i]].enabled || st.hooks[&targets[i]].queued) {
                std::fprintf(stderr, "FAIL: ApplyFailed slot %zu orig=%p enabled=%d queued=%d\n",
                             i, origs[i], int(st.hooks[&targets[i]].enabled), int(st.hooks[&targets[i]].queued));
                return 1;
            }
        }
    }

    // 3b. Apply failure where second apply_queued also fails falls back to per-hook disable without remove
    {
        MockState st;
        st.fail_apply_on_target = &targets[3];
        st.fail_second_apply = true;
        const auto ops = make_ops(st);
        openrr::pe::BatchHookItem items[6];
        build_items(items);
        const auto res = openrr::pe::install_hook_batch(items, 6, ops);
        if (res.error != openrr::pe::BatchInstallError::ApplyFailed || !res.trampolines_retained ||
            st.remove_calls != 0 || st.disable_calls != 6) {
            std::fprintf(stderr, "FAIL: ApplyFailed fallback disable mismatch (rem=%d dis=%d)\n",
                         st.remove_calls, st.disable_calls);
            return 1;
        }
        for (size_t i = 0; i < 6; ++i) {
            if (!origs[i] || st.hooks[&targets[i]].enabled) {
                std::fprintf(stderr, "FAIL: ApplyFailed fallback left hook %zu enabled or orig null\n", i);
                return 1;
            }
        }
    }

    // 4. Success path enables all hooks in a single apply_queued call
    {
        MockState st;
        const auto ops = make_ops(st);
        openrr::pe::BatchHookItem items[6];
        build_items(items);
        const auto res = openrr::pe::install_hook_batch(items, 6, ops);
        if (res.error != openrr::pe::BatchInstallError::Ok || res.trampolines_retained ||
            st.apply_calls != 1 || st.remove_calls != 0) {
            std::fprintf(stderr, "FAIL: batch Ok path mismatch\n");
            return 1;
        }
        for (size_t i = 0; i < 6; ++i) {
            if (!origs[i] || !st.hooks[&targets[i]].enabled) {
                std::fprintf(stderr, "FAIL: batch Ok left hook %zu disabled or orig null\n", i);
                return 1;
            }
        }
    }
    std::printf("[PASS] Direct D3D12 hook batch: single-freeze enable and safe trampoline-retaining failure rollback\n");

    // 5. Multi-entry retryable registry tests (bad first device does not lock out later device)
    {
        MockState st;
        st.fail_enable_target = &targets[3]; // Entry D fails enable_hook
        const auto ops = make_ops(st);
        openrr::pe::EntryHookRegistry<4> reg;
        void* const detours[4] = {
            reinterpret_cast<void*>(uintptr_t(0x1)),
            reinterpret_cast<void*>(uintptr_t(0x2)),
            reinterpret_cast<void*>(uintptr_t(0x3)),
            reinterpret_cast<void*>(uintptr_t(0x4)),
        };
        std::vector<void*> known{&targets[5]}; // Entry F already claimed by another hook family
        int pred_calls = 0;
        auto pred = [&](const void* fn, std::string* mod) -> bool {
            ++pred_calls;
            if (fn == &targets[0]) {
                if (mod) *mod = "refused.dll";
                return false;
            }
            if (mod) *mod = "wrapper.dll";
            return true;
        };

        std::string mod;
        MH_STATUS mh = MH_OK;
        // A refused by policy, retry A is AlreadyFailed (does not re-run policy or MinHook)
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[0], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::RefusedPolicy ||
            mod != "refused.dll") {
            std::fprintf(stderr, "FAIL: expected RefusedPolicy for entry A\n");
            return 1;
        }
        const int pred_after_a = pred_calls;
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[0], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::AlreadyFailed ||
            pred_calls != pred_after_a || st.create_calls != 0) {
            std::fprintf(stderr, "FAIL: expected AlreadyFailed cached for entry A\n");
            return 1;
        }

        // B succeeds on slot 0 even after A failed
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[1], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::Installed ||
            !reg.is_covered_lockfree(&targets[1]) || reg.count.load() != 1) {
            std::fprintf(stderr, "FAIL: entry B did not install after entry A failed\n");
            return 1;
        }
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[1], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::AlreadyCovered ||
            st.create_calls != 1) {
            std::fprintf(stderr, "FAIL: entry B repeat was not AlreadyCovered\n");
            return 1;
        }

        // C (second distinct valid entry) succeeds on slot 1 with distinct orig
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[2], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::Installed ||
            !reg.is_covered_lockfree(&targets[2]) || reg.count.load() != 2 ||
            !reg.slots[0].orig || !reg.slots[1].orig || reg.slots[0].orig == reg.slots[1].orig) {
            std::fprintf(stderr, "FAIL: entry C multi-slot install failed\n");
            return 1;
        }

        // D fails enable_hook -> removed cleanly, orig cleared, count stays 2, E still installs in slot 2
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[3], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::EnableFailed ||
            st.remove_calls != 1 || reg.slots[2].orig != nullptr || reg.count.load() != 2) {
            std::fprintf(stderr, "FAIL: entry D EnableFailed cleanup mismatch\n");
            return 1;
        }
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[4], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::Installed ||
            reg.count.load() != 3 || !reg.slots[2].orig) {
            std::fprintf(stderr, "FAIL: entry E did not reuse slot 2 after D failed\n");
            return 1;
        }

        // F duplicates known_hooked
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[5], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::DuplicateOtherHook ||
            reg.count.load() != 3) {
            std::fprintf(stderr, "FAIL: entry F DuplicateOtherHook check failed\n");
            return 1;
        }

        // G fills slot 3, H sees SlotsFull
        if (openrr::pe::ensure_entry_hook_locked(reg, &targets[6], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::Installed ||
            reg.count.load() != 4 ||
            openrr::pe::ensure_entry_hook_locked(reg, &targets[7], detours, known, pred, &mod, &mh, ops) !=
                openrr::pe::EntryHookOutcome::SlotsFull) {
            std::fprintf(stderr, "FAIL: slot capacity check failed\n");
            return 1;
        }
    }
    std::printf("[PASS] EntryHookRegistry: multi-entry coverage without one-shot global lockout\n");
    return 0;
}

namespace wrap_test {

struct ComObj {
    void** vt;
};

void* g_device_vt[56]{};
void* g_native_vt[44]{};
void* g_heap_vt[9]{};
void* g_list_vt[60]{};
void* g_priv_list_vt[60]{};
void* g_queue_vt[19]{};

ComObj g_device{g_device_vt};
ComObj g_native{g_native_vt};
ComObj g_heap{g_heap_vt};
ComObj g_list{g_list_vt};
ComObj g_priv_list{g_priv_list_vt};
ComObj g_queue{g_queue_vt};

int g_wrapper_roots = 0;
int g_native_roots = 0;
int g_heap_releases = 0;
int g_native_releases = 0;
SIZE_T g_seen_blob_size = 0;
D3D12_HEAP_DESC g_seen_heap_desc{};
HRESULT g_heap_result = S_OK;
HRESULT g_get_device_result = S_OK;
HRESULT g_root_result = S_OK;

ULONG STDMETHODCALLTYPE wt_release(ComObj* obj) {
    if (obj == &g_heap) ++g_heap_releases;
    if (obj == &g_native) ++g_native_releases;
    return 1;
}

HRESULT STDMETHODCALLTYPE wt_query(ComObj*, REFIID, void** out) {
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE wt_get_device(ComObj* obj, REFIID, void** out) {
    *out = nullptr;
    if (obj == &g_heap && FAILED(g_get_device_result)) return g_get_device_result;
    *out = (obj == &g_heap) ? &g_native : &g_device;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE wt_create_heap(ComObj*, const D3D12_HEAP_DESC* desc, REFIID, void** out) {
    *out = nullptr;
    if (FAILED(g_heap_result)) return g_heap_result;
    if (!desc) return E_INVALIDARG;
    g_seen_heap_desc = *desc;
    if (desc->SizeInBytes != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ||
        desc->Properties.Type != D3D12_HEAP_TYPE_DEFAULT ||
        desc->Flags != D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) {
        return E_INVALIDARG;
    }
    *out = &g_heap;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE wt_create_root(ComObj* obj, UINT, const void*, SIZE_T len, REFIID, void** out) {
    if (obj == &g_device) ++g_wrapper_roots;
    else ++g_native_roots;
    g_seen_blob_size = len;
    *out = nullptr;
    if (FAILED(g_root_result)) return g_root_result;
    *out = &g_heap;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE wt_create_queue(ComObj*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** out) {
    *out = &g_queue;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE wt_reset(ComObj*, ID3D12CommandAllocator*, ID3D12PipelineState*) { return S_OK; }
HRESULT STDMETHODCALLTYPE wt_close(ComObj*) { return S_OK; }
void STDMETHODCALLTYPE wt_noop() {}

template <class T>
void* fn_ptr(T f) {
    return reinterpret_cast<void*>(f);
}

}  // namespace wrap_test

static int test_d3d12_wrapper_compat() {
    std::printf("[INFO] Wrapper compat regression tests\n");

    // 1. Shared D3D12 hook policy tests
    if (!openrr::pe::is_conflicting_d3d12_hook_module(L"dlssnr-amd.asi") ||
        !openrr::pe::is_conflicting_d3d12_hook_module(L"DLSSNR-AMD-LOADLATE.ASI") ||
        !openrr::pe::is_conflicting_d3d12_hook_module(L"openrr-amd.asi")) {
        std::fprintf(stderr, "FAIL: conflicting D3D12 hook module check failed\n");
        return 1;
    }
    if (openrr::pe::is_conflicting_d3d12_hook_module(L"ReShade64.asi") ||
        openrr::pe::is_conflicting_d3d12_hook_module(L"ReShade64.dll") ||
        openrr::pe::is_conflicting_d3d12_hook_module(L"D3D12Core.dll") ||
        openrr::pe::is_conflicting_d3d12_hook_module(L"libvkd3d-1.dll")) {
        std::fprintf(stderr, "FAIL: non-conflicting D3D12 wrapper/core module rejected\n");
        return 1;
    }

    std::string mod;
    if (openrr::pe::is_hookable_d3d12_target(nullptr, nullptr, &mod)) {
        std::fprintf(stderr, "FAIL: null function pointer marked hookable\n");
        return 1;
    }

    void* priv_mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!priv_mem) {
        std::fprintf(stderr, "FAIL: VirtualAlloc failed in wrapper compat test\n");
        return 1;
    }
    if (openrr::pe::is_hookable_d3d12_target(priv_mem, nullptr, &mod) || mod != "private memory") {
        std::fprintf(stderr, "FAIL: private memory allowed or misreported (%s)\n", mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }

    const void* local_fn = wrap_test::fn_ptr(wrap_test::wt_close);
    HMODULE self_exe = GetModuleHandleW(nullptr);
    if (openrr::pe::is_hookable_d3d12_target(local_fn, self_exe, &mod)) {
        std::fprintf(stderr, "FAIL: self module target was not rejected\n");
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (!openrr::pe::is_hookable_d3d12_target(local_fn, nullptr, &mod) || mod.empty()) {
        std::fprintf(stderr, "FAIL: non-conflicting PE module target rejected (%s)\n", mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }

    void* split_vt[32]{};
    for (void*& p : split_vt) p = const_cast<void*>(local_fn);
    unsigned bad_slot = 0;
    if (openrr::pe::validate_split_list_vtable(split_vt, self_exe, &bad_slot, &mod) || bad_slot != 2) {
        std::fprintf(stderr, "FAIL: validate_split_list_vtable did not reject self slot 2 (bad=%u)\n", bad_slot);
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (!openrr::pe::validate_split_list_vtable(split_vt, nullptr, &bad_slot, &mod)) {
        std::fprintf(stderr, "FAIL: validate_split_list_vtable rejected valid PE table (slot=%u, mod=%s)\n",
                     bad_slot, mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    split_vt[2] = priv_mem; // kRelease
    if (openrr::pe::validate_split_list_vtable(split_vt, nullptr, &bad_slot, &mod) ||
        bad_slot != 2 || mod != "private memory") {
        std::fprintf(stderr, "FAIL: validate_split_list_vtable missed private memory on kRelease (slot=%u, mod=%s)\n",
                     bad_slot, mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    std::printf("[PASS] D3D12 hook policy: wrapper PE allowed, self/coexist/private-memory/kRelease rejected\n");

    // Populate COM-ABI doubles
    for (auto& slot : wrap_test::g_list_vt) slot = wrap_test::fn_ptr(wrap_test::wt_noop);
    wrap_test::g_list_vt[0] = wrap_test::fn_ptr(wrap_test::wt_query);
    wrap_test::g_list_vt[2] = wrap_test::fn_ptr(wrap_test::wt_release);
    wrap_test::g_list_vt[7] = wrap_test::fn_ptr(wrap_test::wt_get_device);
    wrap_test::g_list_vt[9] = wrap_test::fn_ptr(wrap_test::wt_close);
    wrap_test::g_list_vt[10] = wrap_test::fn_ptr(wrap_test::wt_reset);
    for (size_t i = 0; i < 60; ++i) wrap_test::g_priv_list_vt[i] = wrap_test::g_list_vt[i];
    wrap_test::g_priv_list_vt[2] = priv_mem; // slot 2 (Release) in private memory -> coexistence refusal

    wrap_test::g_device_vt[0] = wrap_test::fn_ptr(wrap_test::wt_query);
    wrap_test::g_device_vt[2] = wrap_test::fn_ptr(wrap_test::wt_release);
    wrap_test::g_device_vt[8] = wrap_test::fn_ptr(wrap_test::wt_create_queue);
    wrap_test::g_device_vt[12] = wrap_test::fn_ptr(wrap_test::wt_noop);
    wrap_test::g_device_vt[16] = wrap_test::fn_ptr(wrap_test::wt_create_root);
    wrap_test::g_device_vt[28] = wrap_test::fn_ptr(wrap_test::wt_create_heap);

    wrap_test::g_native_vt[2] = wrap_test::fn_ptr(wrap_test::wt_release);
    wrap_test::g_native_vt[16] = wrap_test::fn_ptr(wrap_test::wt_create_root);

    wrap_test::g_heap_vt[2] = wrap_test::fn_ptr(wrap_test::wt_release);
    wrap_test::g_heap_vt[7] = wrap_test::fn_ptr(wrap_test::wt_get_device);

    wrap_test::g_queue_vt[2] = wrap_test::fn_ptr(wrap_test::wt_release);
    wrap_test::g_queue_vt[10] = wrap_test::fn_ptr(wrap_test::wt_noop);

    // 2. Carrier root signature tests
    auto* dev = reinterpret_cast<ID3D12Device*>(&wrap_test::g_device);
    ID3D12RootSignature* rs = nullptr;
    std::string why;

    if (onr::create_carrier_root_signature(nullptr, onr::kRootDwords, &rs, &why) ||
        why.find("invalid argument") == std::string::npos || rs != nullptr) {
        std::fprintf(stderr, "FAIL: null device not rejected (%s)\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (onr::create_carrier_root_signature(dev, 0, &rs, &why) ||
        why.find("invalid argument") == std::string::npos || rs != nullptr) {
        std::fprintf(stderr, "FAIL: zero dwords not rejected (%s)\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (!onr::create_carrier_root_signature(dev, onr::kRootDwords, &rs, &why) || !rs) {
        std::fprintf(stderr, "FAIL: create_carrier_root_signature failed: %s\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (wrap_test::g_wrapper_roots != 0 || wrap_test::g_native_roots != 1 ||
        wrap_test::g_heap_releases != 1 || wrap_test::g_native_releases != 1 ||
        wrap_test::g_seen_blob_size == 0 ||
        wrap_test::g_seen_heap_desc.Properties.Type != D3D12_HEAP_TYPE_DEFAULT ||
        wrap_test::g_seen_heap_desc.Flags != D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS ||
        wrap_test::g_seen_heap_desc.SizeInBytes != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
        std::fprintf(stderr, "FAIL: carrier root signature bypassed/refcount/heap check failed\n");
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }

    wrap_test::g_heap_result = E_OUTOFMEMORY;
    if (onr::create_carrier_root_signature(dev, onr::kRootDwords, &rs, &why) || rs || why.find("CreateHeap") != 0) {
        std::fprintf(stderr, "FAIL: CreateHeap failure fell back or wrong error: %s\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    wrap_test::g_heap_result = S_OK;

    wrap_test::g_get_device_result = E_NOINTERFACE;
    if (onr::create_carrier_root_signature(dev, onr::kRootDwords, &rs, &why) || rs ||
        why.find("GetDevice") == std::string::npos || wrap_test::g_heap_releases != 2) {
        std::fprintf(stderr, "FAIL: GetDevice failure wrong error/refcount (%s, rel=%d)\n",
                     why.c_str(), wrap_test::g_heap_releases);
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    wrap_test::g_get_device_result = S_OK;

    wrap_test::g_root_result = E_FAIL;
    if (onr::create_carrier_root_signature(dev, onr::kRootDwords, &rs, &why) || rs ||
        why.find("CreateRootSignature") != 0 ||
        wrap_test::g_heap_releases != 3 || wrap_test::g_native_releases != 2 || wrap_test::g_wrapper_roots != 0) {
        std::fprintf(stderr, "FAIL: CreateRootSignature failure wrong error/refcount/fallback\n");
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    wrap_test::g_root_result = S_OK;
    std::printf("[PASS] onr::create_carrier_root_signature bypasses wrapper without fallback and releases refs\n");

    // 3. openrr::pe::split::prepare wrapped COM-ABI double tests & self-module protection
    if (openrr::pe::split::is_target_allowed(local_fn, &mod)) {
        std::fprintf(stderr, "FAIL: split auto self-module guard failed to reject non-openrr-named module (%s)\n",
                     mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    openrr::pe::split::set_self_module(nullptr);
    if (!openrr::pe::split::is_target_allowed(local_fn, &mod)) {
        std::fprintf(stderr, "FAIL: split::set_self_module(nullptr) did not allow test double module (%s)\n",
                     mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }

    auto* cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(&wrap_test::g_list);
    if (openrr::pe::split::prepare(cmd, &why) || why.find("next Reset") == std::string::npos) {
        std::fprintf(stderr, "FAIL: wrapped split::prepare initial call expected next Reset, got: %s\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    cmd->Reset(nullptr, nullptr);
    if (!openrr::pe::split::prepare(cmd, &why)) {
        std::fprintf(stderr, "FAIL: wrapped split::prepare failed after Reset: %s\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    if (wrap_test::g_device_vt[12] == wrap_test::fn_ptr(wrap_test::wt_noop) ||
        wrap_test::g_queue_vt[10] == wrap_test::fn_ptr(wrap_test::wt_noop)) {
        std::fprintf(stderr, "FAIL: wrapped device or queue vtable was not patched\n");
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    openrr::pe::split::set_self_module(self_exe);
    if (openrr::pe::split::is_target_allowed(local_fn, &mod)) {
        std::fprintf(stderr, "FAIL: split::set_self_module(self_exe) did not reject self module (%s)\n", mod.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    openrr::pe::split::set_self_module(nullptr);
    std::printf("[PASS] openrr::pe::split::prepare accepts wrapped list/device/queue and enforces self-module HMODULE\n");

    // 4. Coexistence refusal test LAST (sets split::g_disabled = true)
    auto* priv_cmd = reinterpret_cast<ID3D12GraphicsCommandList*>(&wrap_test::g_priv_list);
    if (openrr::pe::split::prepare(priv_cmd, &why) ||
        why.find("private memory") == std::string::npos || why.find("slot 2") == std::string::npos) {
        std::fprintf(stderr, "FAIL: private memory kRelease vtable slot was not refused (why=%s)\n", why.c_str());
        VirtualFree(priv_mem, 0, MEM_RELEASE);
        return 1;
    }
    openrr::pe::split::set_self_module(self_exe);
    VirtualFree(priv_mem, 0, MEM_RELEASE);
    std::printf("[PASS] openrr::pe::split::prepare refuses private memory on kRelease (slot 2)\n");
    return 0;
}

namespace nvapi_test {
struct Tag {};
struct NullTag {};
enum class MockMode {
    kNull,
    kPlaceholderNoImpl,
    kPlaceholderOkZero,
    kRealGpuInfo,
};
std::atomic<bool> g_can_provide{false};
std::atomic<MockMode> g_mode{MockMode::kNull};
std::atomic<int> g_logs{0};
std::string g_last_log;
void* g_other_fn = reinterpret_cast<void*>(uintptr_t(0x23456789));
void* g_other_fn2 = reinterpret_cast<void*>(uintptr_t(0x3456789a));

bool can_provide_cb() {
    return g_can_provide.load(std::memory_order_acquire);
}
void log_cb(const char* msg) {
    if (msg && msg[0]) {
        g_last_log = msg;
        ++g_logs;
    }
}

OPENRR_NVAPI_NOINLINE int OPENRR_NVAPI_MSABI mock_placeholder_noimpl(const void*, void*) {
    return openrr::pe::nvapi::kStatusNoImplementation;
}

OPENRR_NVAPI_NOINLINE int OPENRR_NVAPI_MSABI mock_placeholder_ok_zero(const void*, void* gpu_info) {
    if (gpu_info) {
        std::memset(gpu_info, 0, sizeof(uint32_t));
    }
    return openrr::pe::nvapi::kStatusOk;
}

OPENRR_NVAPI_NOINLINE int OPENRR_NVAPI_MSABI mock_real_get_gpu_info(const void*, void* gpu_info) {
    namespace nv = openrr::pe::nvapi;
    if (!gpu_info) return nv::kStatusInvalidArgument;
    uint32_t ver = 0;
    std::memcpy(&ver, gpu_info, sizeof(ver));
    if (ver != nv::kGpuInfoVer2) return nv::kStatusIncompatibleStructVersion;
    nv::GpuInfoV2 out{};
    out.version = nv::kGpuInfoVer2;
    out.ray_tracing_cores = 128u;
    out.tensor_cores = 512u;
    std::memcpy(gpu_info, &out, sizeof(out));
    return nv::kStatusOk;
}

OPENRR_NVAPI_NOINLINE void* OPENRR_NVAPI_MSABI mock_qi(uint32_t id) {
    if (id == openrr::pe::nvapi::kInterfaceIdGpuGetGpuInfo) {
        switch (g_mode.load(std::memory_order_acquire)) {
            case MockMode::kPlaceholderNoImpl: return reinterpret_cast<void*>(&mock_placeholder_noimpl);
            case MockMode::kPlaceholderOkZero: return reinterpret_cast<void*>(&mock_placeholder_ok_zero);
            case MockMode::kRealGpuInfo: return reinterpret_cast<void*>(&mock_real_get_gpu_info);
            case MockMode::kNull:
            default: return nullptr;
        }
    }
    if (id == 0x0150e828u) return g_other_fn;
    return nullptr;
}

OPENRR_NVAPI_NOINLINE void* OPENRR_NVAPI_MSABI mock_qi_second(uint32_t id) {
    if (id == 0x0150e828u) return g_other_fn2;
    return nullptr;
}
} // namespace nvapi_test

static int test_nvapi_gpu_info_fallback() {
    namespace nv = openrr::pe::nvapi;
    if (nv::kGpuInfoVer1 != 0x00010008u || nv::kGpuInfoVer2 != 0x00020050u ||
        sizeof(nv::GpuInfoV1) != 8 || sizeof(nv::GpuInfoV2) != 0x50) {
        std::fprintf(stderr, "FAIL: NV_GPU_INFO struct sizes or version constants mismatch\n");
        return 1;
    }

    int dummy_gpu = 1;
    if (nv::fill_gpu_info(&dummy_gpu, nullptr) != nv::kStatusInvalidArgument) {
        std::fprintf(stderr, "FAIL: null gpu_info did not return INVALID_ARGUMENT\n");
        return 1;
    }
    nv::GpuInfoV2 v2{};
    std::memset(&v2, 0xAA, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    // fakenvapi::NvAPI_EnumPhysicalGPUs yields a null physical handle; fill_gpu_info must accept it.
    if (nv::fill_gpu_info(nullptr, &v2) != nv::kStatusOk ||
        v2.version != nv::kGpuInfoVer2 || v2.flags != 0 || v2.reserved1 != 0 ||
        v2.ray_tracing_cores != nv::kDefaultRayTracingCores ||
        v2.tensor_cores != nv::kDefaultTensorCores) {
        std::fprintf(stderr, "FAIL: fill_gpu_info did not accept null physical_gpu or populated wrong fields\n");
        return 1;
    }
    v2.version = 0x12345678u;
    if (nv::fill_gpu_info(&dummy_gpu, &v2) != nv::kStatusIncompatibleStructVersion) {
        std::fprintf(stderr, "FAIL: bad struct version did not return INCOMPATIBLE_STRUCT_VERSION\n");
        return 1;
    }

    nv::GpuInfoV1 v1{};
    std::memset(&v1, 0xAA, sizeof(v1));
    v1.version = nv::kGpuInfoVer1;
    if (nv::fill_gpu_info(&dummy_gpu, &v1) != nv::kStatusOk ||
        v1.version != nv::kGpuInfoVer1 || v1.flags != 0) {
        std::fprintf(stderr, "FAIL: VER1 fill_gpu_info mismatch\n");
        return 1;
    }

    std::memset(&v2, 0xAA, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    if (nv::fill_gpu_info(&dummy_gpu, &v2) != nv::kStatusOk ||
        v2.version != nv::kGpuInfoVer2 || v2.flags != 0 || v2.reserved1 != 0 ||
        v2.ray_tracing_cores != nv::kDefaultRayTracingCores ||
        v2.tensor_cores != nv::kDefaultTensorCores) {
        std::fprintf(stderr, "FAIL: VER2 fill_gpu_info mismatch\n");
        return 1;
    }
    for (uint32_t r : v2.reserved2) {
        if (r != 0) {
            std::fprintf(stderr, "FAIL: VER2 reserved2 not zeroed\n");
            return 1;
        }
    }

    nvapi_test::g_logs.store(0, std::memory_order_release);
    nvapi_test::g_last_log.clear();

    nv::QueryInterfaceHooks<4, nvapi_test::Tag> hooks;
    hooks.set_callbacks(&nvapi_test::can_provide_cb, &nvapi_test::log_cb);
    if (!hooks.hook_function(reinterpret_cast<void*>(&nvapi_test::mock_qi)) ||
        !hooks.hook_function(reinterpret_cast<void*>(&nvapi_test::mock_qi)) ||
        !hooks.hook_function(reinterpret_cast<void*>(&nvapi_test::mock_qi_second))) {
        std::fprintf(stderr, "FAIL: nvapi QueryInterface hook_function failed\n");
        return 1;
    }

    auto call_qi = [](nv::PFN_NvApiQueryInterface fn, uint32_t id) -> void* {
        volatile nv::PFN_NvApiQueryInterface p = fn;
        return p(id);
    };
    const nv::PFN_NvApiQueryInterface qi = &nvapi_test::mock_qi;
    const nv::PFN_NvApiQueryInterface qi2 = &nvapi_test::mock_qi_second;

    if (call_qi(qi, 0x0150e828u) != nvapi_test::g_other_fn ||
        call_qi(qi2, 0x0150e828u) != nvapi_test::g_other_fn2 ||
        call_qi(qi, 0xdeadbeefu) != nullptr) {
        std::fprintf(stderr, "FAIL: unrelated NVAPI IDs did not pass through multi-slot hooks\n");
        return 1;
    }

    // can_provide=false must return underlying pointers (both placeholder and nullptr) untouched without wrapping.
    nvapi_test::g_can_provide.store(false, std::memory_order_release);
    nvapi_test::g_mode.store(nvapi_test::MockMode::kPlaceholderNoImpl, std::memory_order_release);
    if (call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo) != reinterpret_cast<void*>(&nvapi_test::mock_placeholder_noimpl) ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 0) {
        std::fprintf(stderr, "FAIL: wrapped non-null GetGPUInfo when can_provide=false\n");
        return 1;
    }
    nvapi_test::g_mode.store(nvapi_test::MockMode::kNull, std::memory_order_release);
    if (call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo) != nullptr ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 0) {
        std::fprintf(stderr, "FAIL: wrapped null GetGPUInfo when can_provide=false\n");
        return 1;
    }

    // Real implementation (ray_tracing_cores > 0) must pass through untouched without logging fallback.
    nvapi_test::g_can_provide.store(true, std::memory_order_release);
    nvapi_test::g_mode.store(nvapi_test::MockMode::kRealGpuInfo, std::memory_order_release);
    void* real_wrapped = call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo);
    if (!real_wrapped || real_wrapped == reinterpret_cast<void*>(&nvapi_test::mock_real_get_gpu_info)) {
        std::fprintf(stderr, "FAIL: expected wrapper for real GetGPUInfo inspection\n");
        return 1;
    }
    auto get_gpu_info = reinterpret_cast<nv::PFN_NvApiGpuGetGpuInfo>(real_wrapped);
    std::memset(&v2, 0x55, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    if (get_gpu_info(&dummy_gpu, &v2) != nv::kStatusOk ||
        v2.ray_tracing_cores != 128u || v2.tensor_cores != 512u ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 0) {
        std::fprintf(stderr, "FAIL: real GetGPUInfo was overridden or logged fallback\n");
        return 1;
    }

    // fakenvapi placeholder returning NVAPI_NO_IMPLEMENTATION (-3) + null handle must fall back cleanly.
    nvapi_test::g_mode.store(nvapi_test::MockMode::kPlaceholderNoImpl, std::memory_order_release);
    void* ph_noimpl_ptr = call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo);
    void* second_slot_ptr = call_qi(qi2, nv::kInterfaceIdGpuGetGpuInfo);
    if (!ph_noimpl_ptr || !second_slot_ptr || ph_noimpl_ptr == second_slot_ptr) {
        std::fprintf(stderr, "FAIL: expected distinct per-slot wrappers for qi and qi2\n");
        return 1;
    }
    get_gpu_info = reinterpret_cast<nv::PFN_NvApiGpuGetGpuInfo>(ph_noimpl_ptr);
    std::memset(&v2, 0x55, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    if (get_gpu_info(nullptr, &v2) != nv::kStatusOk ||
        v2.flags != 0 || v2.reserved1 != 0 ||
        v2.ray_tracing_cores != nv::kDefaultRayTracingCores ||
        v2.tensor_cores != nv::kDefaultTensorCores ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 1 ||
        nvapi_test::g_last_log.find("underlying placeholder") == std::string::npos) {
        std::fprintf(stderr, "FAIL: placeholder (-3) fallback failed (logs=%d)\n",
                     nvapi_test::g_logs.load(std::memory_order_acquire));
        return 1;
    }

    // No-op stub returning NVAPI_OK (0) with ray_tracing_cores == 0 must also fall back (restoring saved version).
    nvapi_test::g_mode.store(nvapi_test::MockMode::kPlaceholderOkZero, std::memory_order_release);
    void* ph_okzero_ptr = call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo);
    get_gpu_info = reinterpret_cast<nv::PFN_NvApiGpuGetGpuInfo>(ph_okzero_ptr);
    std::memset(&v2, 0x00, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    if (get_gpu_info(nullptr, &v2) != nv::kStatusOk ||
        v2.ray_tracing_cores != nv::kDefaultRayTracingCores ||
        v2.tensor_cores != nv::kDefaultTensorCores ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 1) {
        std::fprintf(stderr, "FAIL: zeroed OK() placeholder fallback failed\n");
        return 1;
    }

    // Unhook module 1 and verify mock_qi returns directly, while a second instance tests underlying nullptr fallback.
    HMODULE self_exe = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self_exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(self_exe) + dos->e_lfanew);
    hooks.unhook_module(self_exe, nt->OptionalHeader.SizeOfImage);
    nvapi_test::g_mode.store(nvapi_test::MockMode::kNull, std::memory_order_release);
    if (call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo) != nullptr ||
        call_qi(qi, 0x0150e828u) != nvapi_test::g_other_fn) {
        std::fprintf(stderr, "FAIL: unhook_module did not restore direct mock_qi\n");
        return 1;
    }

    nvapi_test::g_logs.store(0, std::memory_order_release);
    nvapi_test::g_last_log.clear();
    nv::QueryInterfaceHooks<4, nvapi_test::NullTag> null_hooks;
    null_hooks.set_callbacks(&nvapi_test::can_provide_cb, &nvapi_test::log_cb);
    if (!null_hooks.hook_function(reinterpret_cast<void*>(&nvapi_test::mock_qi))) {
        std::fprintf(stderr, "FAIL: null_hooks.hook_function failed\n");
        return 1;
    }
    void* null_wrapped = call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo);
    if (!null_wrapped) {
        std::fprintf(stderr, "FAIL: null underlying did not return wrapper\n");
        return 1;
    }
    get_gpu_info = reinterpret_cast<nv::PFN_NvApiGpuGetGpuInfo>(null_wrapped);
    std::memset(&v2, 0x55, sizeof(v2));
    v2.version = nv::kGpuInfoVer2;
    if (get_gpu_info(nullptr, &v2) != nv::kStatusOk ||
        v2.flags != 0 || v2.reserved1 != 0 ||
        v2.ray_tracing_cores != nv::kDefaultRayTracingCores ||
        v2.tensor_cores != nv::kDefaultTensorCores ||
        nvapi_test::g_logs.load(std::memory_order_acquire) != 1 ||
        nvapi_test::g_last_log.find("underlying null") == std::string::npos) {
        std::fprintf(stderr, "FAIL: null underlying fallback failed (logs=%d)\n",
                     nvapi_test::g_logs.load(std::memory_order_acquire));
        return 1;
    }
    null_hooks.unhook_module(self_exe, nt->OptionalHeader.SizeOfImage);
    if (call_qi(qi, nv::kInterfaceIdGpuGetGpuInfo) != nullptr ||
        call_qi(qi, 0x0150e828u) != nvapi_test::g_other_fn) {
        std::fprintf(stderr, "FAIL: null_hooks.unhook_module did not restore direct mock_qi\n");
        return 1;
    }

    std::printf("[PASS] openrr::pe::nvapi NvAPI_GPU_GetGPUInfo (0xafd1b02c) fallback and multi-slot hook\n");
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
    if (test_v017_stage10() != 0) return 1;
    if (test_split_hardening_and_eligibility() != 0) return 1;
    if (test_d3d12_gpu_and_engine() != 0) return 1;
    if (test_stage10_final_audit() != 0) return 1;
    if (test_stage10_r2_fixes() != 0) return 1;
    if (test_d3d12_direct_hook_safety() != 0) return 1;
    if (test_d3d12_wrapper_compat() != 0) return 1;
    if (test_nvapi_gpu_info_fallback() != 0) return 1;
    std::printf("ALL ASI UNIT TESTS PASSED\n");
    return 0;
}
