// SPDX-License-Identifier: MIT
// Standalone offline benchmark and VRAM/DRS comparator for OpenNrRrRuntime.dll and openrr-amd.asi.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "opennr_rr_api.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
constexpr D3D12_RESOURCE_STATES kReadState = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

uint16_t FloatToHalf(float v)
{
    const uint32_t u = std::bit_cast<uint32_t>(v);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const int32_t exp = int32_t((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;
    if (exp <= 0)
    {
        if (exp < -10) return uint16_t(sign);
        mant = (mant | 0x800000u) >> (1 - exp);
        return uint16_t(sign | ((mant + 0x1000u) >> 13));
    }
    if (exp >= 31) return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exp) << 10) | ((mant + 0x1000u) >> 13));
}

void Require(bool ok, const std::string& msg)
{
    if (!ok)
        throw std::runtime_error(msg);
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

void WaitForFence(ID3D12Device* dev, ID3D12Fence* fence, UINT64 fv, const char* where)
{
    if (fence->GetCompletedValue() < fv)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(ev != nullptr, std::string(where) + ": CreateEventW failed");
        HRESULT hr = fence->SetEventOnCompletion(fv, ev);
        if (FAILED(hr))
        {
            CloseHandle(ev);
            Check(hr, where);
        }
        DWORD w = WaitForSingleObject(ev, 60000);
        CloseHandle(ev);
        Require(w == WAIT_OBJECT_0, std::string(where) + ": WaitForSingleObject timed out or failed");
    }
    Require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= fv,
            std::string(where) + ": fence did not complete or device was removed");
    Check(dev->GetDeviceRemovedReason(), where);
}

void ValidateOutputPixels(const std::vector<uint16_t>& px, const std::string& phase)
{
    Require(!px.empty(), phase + ": output pixel buffer is empty");
    size_t nonZero = 0;
    for (size_t i = 0; i < px.size(); ++i)
    {
        const uint16_t h = px[i];
        Require((h & 0x7c00u) != 0x7c00u, phase + ": output contains NaN/Inf half-float");
        if ((i % 4) != 3 && (h & 0x7fffu) != 0)
            ++nonZero;
    }
    Require(nonZero > px.size() / 4, phase + ": output image is unexpectedly blank/zero");
}

uint64_t QueryProcessVramBytes(IDXGIAdapter3* adapter)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return info.CurrentUsage;
    return 0;
}

double ParseFeatureSelfMbFromLog(const fs::path& logPath)
{
    std::ifstream f(logPath);
    if (!f)
        return 0.0;
    std::string line;
    double lastMb = 0.0;
    std::regex re("VRAM: this feature ([0-9.]+) MB");
    while (std::getline(f, line))
    {
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size() > 1)
            lastMb = std::stod(m[1].str());
    }
    return lastMb;
}

struct GpuTex
{
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t w = 0, h = 0;

    void Create(ID3D12Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES initState)
    {
        w = width;
        h = height;
        state = initState;
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, IID_PPV_ARGS(&res)),
              "CreateCommittedResource(Tex)");
    }
};

std::vector<uint16_t> ReadbackTextureHalf(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* ca,
                                          ID3D12GraphicsCommandList* cl, ID3D12Fence* fence, UINT64& fv, GpuTex& tex,
                                          void (*notifyAllocReset)(void*) = nullptr,
                                          void (*notifyListReset)(void*, void*) = nullptr)
{
    D3D12_RESOURCE_DESC desc = tex.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)),
          "Create readback");

    Check(ca->Reset(), "Reset allocator");
    if (notifyAllocReset) notifyAllocReset(ca);
    Check(cl->Reset(ca, nullptr), "Reset list");
    if (notifyListReset) notifyListReset(cl, ca);
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, tex.state, D3D12_RESOURCE_STATE_COPY_SOURCE };
    cl->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src { tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    Check(cl->Close(), "Close");
    ID3D12CommandList* lists[] = { cl };
    q->ExecuteCommandLists(1, lists);
    Check(q->Signal(fence, ++fv), "Signal");
    WaitForFence(dev, fence, fv, "ReadbackTextureHalf");

    uint8_t* mapped = nullptr;
    Check(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map readback");
    std::vector<uint16_t> out(size_t(tex.w) * tex.h * 4);
    for (uint32_t y = 0; y < tex.h; ++y)
        std::memcpy(out.data() + size_t(y) * tex.w * 4, mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, tex.w * 8);
    D3D12_RANGE empty {};
    rb->Unmap(0, &empty);
    return out;
}

void UploadPattern(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* ca, ID3D12GraphicsCommandList* cl,
                   ID3D12Fence* fence, UINT64& fv, GpuTex& color, GpuTex& diff, GpuTex& spec, GpuTex& norm,
                   GpuTex& depth, GpuTex& mv, GpuTex& hit)
{
    const uint32_t w = color.w, h = color.h;
    std::vector<uint16_t> cData(size_t(w) * h * 4), nData(size_t(w) * h * 4), mvData(size_t(w) * h * 2);
    std::vector<uint8_t> dData(size_t(w) * h * 4), sData(size_t(w) * h * 4);
    std::vector<float> zData(size_t(w) * h), hData(size_t(w) * h);

    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const float u = float(x) / float(w), v = float(y) / float(h);
            const float checker = (((x / 16) + (y / 16)) & 1) ? 0.75f : 1.0f;
            const float noise = float(((i * 1103515245u + 12345u) >> 16) & 0xff) / 255.0f * 0.15f;
            cData[4 * i + 0] = FloatToHalf((0.4f * checker + noise) * (0.5f + 0.5f * u));
            cData[4 * i + 1] = FloatToHalf((0.35f * checker + noise) * (0.5f + 0.5f * v));
            cData[4 * i + 2] = FloatToHalf(0.25f * checker + noise);
            cData[4 * i + 3] = FloatToHalf(1.0f);

            nData[4 * i + 0] = FloatToHalf(0.0f);
            nData[4 * i + 1] = FloatToHalf(0.0f);
            nData[4 * i + 2] = FloatToHalf(1.0f);
            nData[4 * i + 3] = FloatToHalf(0.3f); // packed roughness

            mvData[2 * i + 0] = FloatToHalf(0.25f);
            mvData[2 * i + 1] = FloatToHalf(-0.15f);

            dData[4 * i + 0] = uint8_t(140 * checker);
            dData[4 * i + 1] = uint8_t(110 * checker);
            dData[4 * i + 2] = uint8_t(80 * checker);
            dData[4 * i + 3] = 255;

            sData[4 * i + 0] = 16;
            sData[4 * i + 1] = 16;
            sData[4 * i + 2] = 16;
            sData[4 * i + 3] = 255;

            zData[i] = 0.02f + 0.8f * u;
            hData[i] = 2.5f + 5.0f * v;
        }
    }

    auto uploadOne = [&](GpuTex& t, const void* raw, uint32_t bpp) {
        D3D12_RESOURCE_DESC desc = t.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> up;
        Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)),
              "Create upload");
        uint8_t* m = nullptr;
        Check(up->Map(0, nullptr, reinterpret_cast<void**>(&m)), "Map upload");
        for (uint32_t y = 0; y < t.h; ++y)
            std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(raw) + size_t(y) * t.w * bpp,
                        size_t(t.w) * bpp);
        up->Unmap(0, nullptr);

        Check(ca->Reset(), "Reset allocator");
        Check(cl->Reset(ca, nullptr), "Reset list");
        if (t.state != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { t.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.state, D3D12_RESOURCE_STATE_COPY_DEST };
            cl->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst { t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        D3D12_TEXTURE_COPY_LOCATION src { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { t.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, kReadState };
            cl->ResourceBarrier(1, &b);
        }
        t.state = kReadState;
        Check(cl->Close(), "Close");
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        Check(q->Signal(fence, ++fv), "Signal");
        WaitForFence(dev, fence, fv, "UploadPattern");
    };

    uploadOne(color, cData.data(), 8);
    uploadOne(norm, nData.data(), 8);
    uploadOne(mv, mvData.data(), 4);
    uploadOne(diff, dData.data(), 4);
    uploadOne(spec, sData.data(), 4);
    uploadOne(depth, zData.data(), 4);
    uploadOne(hit, hData.data(), 4);
}

struct Stats
{
    double mean = 0.0, median = 0.0, p95 = 0.0, max = 0.0;
};

Stats ComputeStats(std::vector<double> v)
{
    if (v.empty())
        return {};
    std::sort(v.begin(), v.end());
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    size_t n = v.size();
    return {
        sum / double(n),
        v[n / 2],
        v[std::min(n - 1, size_t(double(n) * 0.95))],
        v.back()
    };
}

struct BenchResult
{
    std::string label;
    Stats steadyGpuMs {};
    Stats drsGpuMs {};
    double runtimeReportedAvgMs = 0.0;
    double featureSelfMbSteady = 0.0;
    double featureSelfMbDrs = 0.0;
    double processVramDeltaMbSteady = 0.0;
    double processVramDeltaMbDrs = 0.0;
    std::vector<uint16_t> steadyPixels;
    std::vector<uint16_t> drsPixels;
    std::vector<fs::path> frameOutputs;
};

BenchResult RunRuntimeBench(const std::string& label, const fs::path& dllPath, const fs::path& dataDir, const fs::path& dlssdPath,
                            const fs::path& outDir, uint32_t maxRenderW, uint32_t maxRenderH, uint32_t outW, uint32_t outH,
                            uint32_t warmupFrames, uint32_t steadyFrames, uint32_t drsFrames, const std::wstring& backendStr = L"d3d12",
                            int32_t fastMode = 0, bool optionalGuides = false, uint32_t compiler = 0, bool parityAllFrames = false,
                            int32_t traceFrame = -1, bool alignPhase = false)
{
    const fs::path absDllPath = fs::absolute(dllPath);
    HMODULE mod = LoadLibraryExW(absDllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Require(mod != nullptr, "Failed to load runtime module: " + absDllPath.string());

    if (auto vkLoad = reinterpret_cast<void(*)()>(GetProcAddress(mod, "rr_vk_load")))
        vkLoad();

    auto getApi = reinterpret_cast<PFN_OnrRrGetApi>(GetProcAddress(mod, "OnrRrGetApi"));
    Require(getApi != nullptr, "Missing OnrRrGetApi export in " + absDllPath.string());
    OnrRrApi4 api4 {};
    Require(getApi(ONR_RR_ABI_VERSION_4, &api4.v3.v2.v1) == ONR_RR_OK, "OnrRrGetApi(v4) failed");
    auto setFastMode = reinterpret_cast<decltype(&OnrRrSetFastMode)>(GetProcAddress(mod, "OnrRrSetFastMode"));
    auto getFastMode = reinterpret_cast<decltype(&OnrRrGetFastMode)>(GetProcAddress(mod, "OnrRrGetFastMode"));
    auto diagnostic = reinterpret_cast<int32_t(*)(void*,uint32_t,uint64_t*)>(GetProcAddress(mod, "OnrRrDiagnostic"));
    Require((traceFrame<0 && !alignPhase) || diagnostic, "Runtime lacks OnrRrDiagnostic");
    auto notifyListReset = reinterpret_cast<void(*)(void*, void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12ListReset"));
    auto notifyAllocReset = reinterpret_cast<void(*)(void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12AllocatorReset"));
    auto notifyListReleased = reinterpret_cast<void(*)(void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12ListReleased"));
    auto notifyAllocReleased = reinterpret_cast<void(*)(void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12AllocatorReleased"));

    ComPtr<IDXGIFactory6> fac;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> ad1;
    ComPtr<IDXGIAdapter3> ad3;
    for (UINT i = 0; fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad1)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        ad1->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            ad1.As(&ad3);
            break;
        }
    }
    Require(ad3 != nullptr, "No AMD hardware adapter found");

    ComPtr<ID3D12Device> dev;
    Check(D3D12CreateDevice(ad3.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> q;
    Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "CreateCommandQueue");
    UINT64 tsFreq = 1;
    Check(q->GetTimestampFrequency(&tsFreq), "GetTimestampFrequency");

    ComPtr<ID3D12CommandAllocator> ca;
    Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> cl;
    Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl)), "CreateCommandList");
    if (notifyListReset) notifyListReset(cl.Get(), ca.Get());
    Check(cl->Close(), "Close initial");

    ComPtr<ID3D12Fence> fence;
    Check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    UINT64 fv = 0;

    D3D12_QUERY_HEAP_DESC qhd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
    ComPtr<ID3D12QueryHeap> qh;
    Check(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "CreateQueryHeap");
    D3D12_HEAP_PROPERTIES rbhp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rbd { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    ComPtr<ID3D12Resource> qrb;
    Check(dev->CreateCommittedResource(&rbhp, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&qrb)),
          "Create timestamp readback");

    GpuTex color, diff, spec, norm, depth, mv, hit, output;
    color.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    norm.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    mv.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    diff.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    spec.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    depth.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    hit.Create(dev.Get(), maxRenderW, maxRenderH, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    output.Create(dev.Get(), outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    UploadPattern(dev.Get(), q.Get(), ca.Get(), cl.Get(), fence.Get(), fv, color, diff, spec, norm, depth, mv, hit);

    const uint64_t vramBeforeCreate = QueryProcessVramBytes(ad3.Get());
    const fs::path logPath = fs::absolute(outDir / ("openrr_" + label + ".log"));
    const fs::path absDataDir = fs::absolute(dataDir);
    const fs::path absDlssdPath = fs::absolute(dlssdPath);
    std::error_code ec;
    fs::remove(logPath, ec);

    OnrRrCreateInfo ci {};
    ci.size = sizeof(ci);
    ci.mode = ONR_RR_MODE_FAST;
    ci.compiler = compiler;
    ci.d3d12_device = dev.Get();
    ci.max_render_width = maxRenderW;
    ci.max_render_height = maxRenderH;
    ci.output_width = outW;
    ci.output_height = outH;
    ci.dlssd_path = absDlssdPath.c_str();
    ci.data_dir = absDataDir.c_str();
    ci.log_path = logPath.c_str();
    const bool wantVulkan = backendStr == L"vulkan";
    ci.backend = wantVulkan ? ONR_RR_BACKEND_CHOICE_VULKAN : ONR_RR_BACKEND_CHOICE_D3D12;

    void* ctx = nullptr;
    Require(api4.v3.v2.v1.Create(&ci, &ctx) == ONR_RR_OK && ctx != nullptr, "OpenRR Create failed");
    if (setFastMode)
        Require(setFastMode(ctx, fastMode) == ONR_RR_OK, "OnrRrSetFastMode failed");
    else if (fastMode != 0)
        throw std::runtime_error("Runtime module does not export OnrRrSetFastMode");

    ComPtr<ID3D12CommandAllocator> ca2;
    Check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca2)), "CreateCommandAllocator2");
    ComPtr<ID3D12GraphicsCommandList> cl2;
    Check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca2.Get(), nullptr, IID_PPV_ARGS(&cl2)), "CreateCommandList2");
    if (notifyListReset) notifyListReset(cl2.Get(), ca2.Get());
    Check(cl2->Close(), "Close initial 2");

    std::ofstream states(outDir / (label+"-frame-state.csv"));
    states<<"call,phase,phase_index,engine_before,engine_after,render_w,render_h,reset,trace\n";
    std::string phaseName="building";uint32_t phaseIndex=0,call=0;int32_t traceIndex=-1;
    auto runOneFrame = [&](uint32_t rw, uint32_t rh, bool reset) -> double {
        uint64_t before=0,after=0;
        if(diagnostic)Require(diagnostic(ctx,0,&before)==ONR_RR_OK,"Diagnostic phase query failed");
        const bool trace=traceFrame>=0 && traceIndex==traceFrame;
        const auto tracePath=fs::absolute(outDir/(label+"-trace-"+std::to_string(traceIndex)));
        SetEnvironmentVariableW(L"ONR_RR_TRACE_PATH",trace?tracePath.c_str():nullptr);
        OnrRrFrame f {};
        f.size = sizeof(f);
        f.flags = ONR_RR_FRAME_HW_DEPTH | ONR_RR_FRAME_DEPTH_INVERTED | ONR_RR_FRAME_ROUGHNESS_PACKED | (reset ? ONR_RR_FRAME_RESET : 0u);
        f.render_width = rw;
        f.render_height = rh;
        f.jitter_x = 0.125f;
        f.jitter_y = -0.25f;
        f.mv_scale_x = 1.0f;
        f.mv_scale_y = 1.0f;
        f.world_to_view[0] = f.world_to_view[5] = f.world_to_view[10] = f.world_to_view[15] = 1.0f;
        f.view_to_clip[0] = 1.0f;
        f.view_to_clip[5] = 1.777f;
        f.view_to_clip[11] = -1.0f;
        f.view_to_clip[14] = 0.05f;
        f.color = { color.res.Get(), kReadState, 0, 0 };
        f.diffuse_albedo = { diff.res.Get(), kReadState, 0, 0 };
        f.specular_albedo = { spec.res.Get(), kReadState, 0, 0 };
        f.normals = { norm.res.Get(), kReadState, 0, 0 };
        f.depth = { depth.res.Get(), kReadState, 0, 0 };
        f.motion = { mv.res.Get(), kReadState, 0, 0 };
        f.specular_hit_distance = { hit.res.Get(), kReadState, 0, 0 };
        f.output = { output.res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0 };
        if (optionalGuides)
        {
            f.transparency_layer = { color.res.Get(), kReadState, 0, 0 };
            f.transparency_opacity = { color.res.Get(), kReadState, 0, 0 };
            f.transparency_motion = { mv.res.Get(), kReadState, 0, 0 };
            f.disocclusion_mask = { hit.res.Get(), kReadState, 0, 0 };
            f.sss_guide = { hit.res.Get(), kReadState, 0, 0 };
            f.responsivity_mask = { hit.res.Get(), kReadState, 0, 0 };
            f.color_before_transparency = { color.res.Get(), kReadState, 0, 0 };
            f.depth_of_field_guide = { hit.res.Get(), kReadState, 0, 0 };
            f.specular_motion = { mv.res.Get(), kReadState, 0, 0 };
        }

        Check(ca->Reset(), "Reset allocator");
        if (notifyAllocReset) notifyAllocReset(ca.Get());
        Check(cl->Reset(ca.Get(), nullptr), "Reset list");
        if (notifyListReset) notifyListReset(cl.Get(), ca.Get());
        cl->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        void* job = nullptr;
        if (!wantVulkan)
        {
            Require(api4.Record(ctx, &f, cl.Get(), ONR_RR_EDIT_OFF, nullptr, &job) == ONR_RR_OK, "Record failed");
            cl->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cl->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
            Check(cl->Close(), "Close");
            ID3D12CommandList* lists[] = { cl.Get() };
            q->ExecuteCommandLists(1, lists);
            Check(q->Signal(fence.Get(), ++fv), "Signal");
            WaitForFence(dev.Get(), fence.Get(), fv, "runOneFrame");
        }
        else
        {
            Require(api4.v3.v2.v1.RecordInputs(ctx, &f, cl.Get(), &job) == ONR_RR_OK, "RecordInputs failed");
            Check(cl->Close(), "Close RecordInputs");
            ID3D12CommandList* lists1[] = { cl.Get() };
            q->ExecuteCommandLists(1, lists1);
            Require(api4.v3.v2.v1.Enqueue(ctx, job, q.Get()) == ONR_RR_OK, "Enqueue failed");
            Check(ca2->Reset(), "Reset allocator 2");
            if (notifyAllocReset) notifyAllocReset(ca2.Get());
            Check(cl2->Reset(ca2.Get(), nullptr), "Reset list 2");
            if (notifyListReset) notifyListReset(cl2.Get(), ca2.Get());
            Require(api4.v3.v2.v1.RecordOutput(ctx, job, cl2.Get()) == ONR_RR_OK, "RecordOutput failed");
            cl2->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cl2->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
            Check(cl2->Close(), "Close RecordOutput");
            ID3D12CommandList* lists2[] = { cl2.Get() };
            q->ExecuteCommandLists(1, lists2);
            Check(q->Signal(fence.Get(), ++fv), "Signal");
            WaitForFence(dev.Get(), fence.Get(), fv, "runOneFrame(Vulkan)");
        }
        SetEnvironmentVariableW(L"ONR_RR_TRACE_PATH",nullptr);
        if(diagnostic)Require(diagnostic(ctx,trace?1:0,&after)==ONR_RR_OK,"Diagnostic readback/phase query failed");
        states<<call++<<','<<phaseName<<','<<phaseIndex++<<','<<before<<','<<after<<','<<rw<<','<<rh<<','<<reset<<','<<trace<<'\n';
        uint64_t ts[2] {};
        void* m = nullptr;
        Check(qrb->Map(0, nullptr, &m), "Map timestamps");
        std::memcpy(ts, m, 16);
        D3D12_RANGE empty {};
        qrb->Unmap(0, &empty);
        return double(ts[1] - ts[0]) * 1000.0 / double(tsFreq);
    };

    for (int waitIter = 0; waitIter < 2400; ++waitIter)
    {
        OnrRrInfo info {};
        info.size = sizeof(info);
        Require(api4.v3.v2.v1.GetInfo(ctx, &info) == ONR_RR_OK, "GetInfo failed");
        Require(info.state != ONR_RR_STATE_FAILED, std::string("OpenRR failed during build: ") + info.message);
        runOneFrame(maxRenderW, maxRenderH, waitIter == 0);
        const bool fastReady = !fastMode || (getFastMode && getFastMode(ctx) == 1);
        Require(!fastMode || !getFastMode || getFastMode(ctx) != 3, "OpenRR fast mode build failed (state 3)");
        if (info.state == ONR_RR_STATE_READY && info.frames >= 1 && fastReady)
            break;
        Sleep(25);
    }

    OnrRrInfo readyInfo {};
    readyInfo.size = sizeof(readyInfo);
    Require(api4.v3.v2.v1.GetInfo(ctx, &readyInfo) == ONR_RR_OK && readyInfo.state == ONR_RR_STATE_READY,
            "OpenRR did not reach ONR_RR_STATE_READY");
    Require(readyInfo.backend == (wantVulkan ? ONR_RR_BACKEND_VULKAN : ONR_RR_BACKEND_D3D12), "Unexpected OpenRR backend");

    // Warmup
    if(alignPhase)Require(diagnostic(ctx,2,nullptr)==ONR_RR_OK,"Diagnostic phase alignment failed");
    phaseName="warmup";phaseIndex=0;traceIndex=0;
    for (uint32_t i = 0; i < warmupFrames; ++i)
    {runOneFrame(maxRenderW, maxRenderH, i == 0);++traceIndex;}

    BenchResult res;
    res.label = label;
    auto captureFrame = [&](const char* phase, uint32_t index) {
        if (!parityAllFrames) return;
        auto pixels = ReadbackTextureHalf(dev.Get(), q.Get(), ca.Get(), cl.Get(), fence.Get(), fv, output, notifyAllocReset, notifyListReset);
        ValidateOutputPixels(pixels, label + " " + phase + " frame " + std::to_string(index));
        auto path = outDir / (label + "-" + phase + "-" + std::to_string(index) + ".bin");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()*sizeof(uint16_t));
        Require(bool(file), "Cannot write per-frame parity output");
        res.frameOutputs.push_back(path);
    };
    // Per-frame readbacks are outside GPU timestamp intervals but perturb
    // scheduling. Use a separate run without this option for throughput.
    // Steady-state measurement at maxRenderW x maxRenderH
    std::vector<double> steadySamples;
    phaseName="steady";phaseIndex=0;
    steadySamples.reserve(steadyFrames);
    for (uint32_t i = 0; i < steadyFrames; ++i)
    {
        steadySamples.push_back(runOneFrame(maxRenderW, maxRenderH, false));
        captureFrame("steady", i);
        ++traceIndex;
    }

    const uint64_t vramSteady = QueryProcessVramBytes(ad3.Get());
    const double selfMbSteady = ParseFeatureSelfMbFromLog(logPath);

    res.steadyPixels = ReadbackTextureHalf(dev.Get(), q.Get(), ca.Get(), cl.Get(), fence.Get(), fv, output, notifyAllocReset, notifyListReset);
    ValidateOutputPixels(res.steadyPixels, label + " steady");

    // DRS measurement: alternate between 85% render scale and 100% render scale every 4 frames
    const uint32_t drsW = (maxRenderW * 85 / 100) & ~1u;
    const uint32_t drsH = (maxRenderH * 85 / 100) & ~1u;
    std::vector<double> drsSamples;
    phaseName="drs";phaseIndex=0;
    drsSamples.reserve(drsFrames);
    for (uint32_t i = 0; i < drsFrames; ++i)
    {
        const bool lower = ((i / 4) & 1) != 0;
        const bool switched = (i % 4) == 0;
        drsSamples.push_back(runOneFrame(lower ? drsW : maxRenderW, lower ? drsH : maxRenderH, switched));
        captureFrame("drs", i);
        ++traceIndex;
    }

    const uint64_t vramDrs = QueryProcessVramBytes(ad3.Get());
    const double selfMbDrs = ParseFeatureSelfMbFromLog(logPath);
    res.drsPixels = ReadbackTextureHalf(dev.Get(), q.Get(), ca.Get(), cl.Get(), fence.Get(), fv, output, notifyAllocReset, notifyListReset);
    ValidateOutputPixels(res.drsPixels, label + " drs");

    OnrRrInfo finalInfo {};
    finalInfo.size = sizeof(finalInfo);
    api4.v3.v2.v1.GetInfo(ctx, &finalInfo);

    res.steadyGpuMs = ComputeStats(steadySamples);
    res.drsGpuMs = ComputeStats(drsSamples);
    res.runtimeReportedAvgMs = finalInfo.gpu_ms_average;
    res.featureSelfMbSteady = selfMbSteady;
    res.featureSelfMbDrs = selfMbDrs;
    res.processVramDeltaMbSteady = vramSteady > vramBeforeCreate ? double(vramSteady - vramBeforeCreate) / (1024.0 * 1024.0) : 0.0;
    res.processVramDeltaMbDrs = vramDrs > vramBeforeCreate ? double(vramDrs - vramBeforeCreate) / (1024.0 * 1024.0) : 0.0;

    Check(ca->Reset(), "Final reset allocator");
    if (notifyAllocReset) notifyAllocReset(ca.Get());
    Check(ca2->Reset(), "Final reset allocator 2");
    if (notifyAllocReset) notifyAllocReset(ca2.Get());
    if (notifyListReleased) {
        notifyListReleased(cl.Get());
        notifyListReleased(cl2.Get());
    }
    if (notifyAllocReleased) {
        notifyAllocReleased(ca.Get());
        notifyAllocReleased(ca2.Get());
    }

    api4.v3.v2.v1.Destroy(ctx);
    const double selfMbAfterDestroy = ParseFeatureSelfMbFromLog(logPath);
    if (res.featureSelfMbSteady <= 0.0) res.featureSelfMbSteady = selfMbAfterDestroy;
    if (res.featureSelfMbDrs <= 0.0) res.featureSelfMbDrs = selfMbAfterDestroy;
    if (label == "baseline")
        Sleep(2200);
    return res;
}
} // namespace

int main()
try
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path runtimePath = L"build/asi/stage/plugins/openrr-amd.asi";
    fs::path baselinePath;
    fs::path dataDir = L"build/asi/stage/openrr-amd";
    fs::path baselineDataDir;
    fs::path dlssdPath = L"C:/Users/Kenny/Downloads/dlssnr_on_amd_setup/dist/weights/nvngx_dlssd.dll";
    fs::path outDir = L"docs/test-evidence/runs/latest";
    uint32_t rw = 1706, rh = 960, ow = 2560, oh = 1440;
    uint32_t warmup = 32, steady = 120, drs = 64;
    std::wstring backendStr = L"d3d12";
    int32_t fastMode = 1;
    bool optionalGuides = false;
    bool parityAllFrames = false;
    int32_t traceFrame=-1;bool alignPhase=false;
    uint32_t baselineCompiler = 1, candidateCompiler = 0;
    auto parseCompiler = [](const std::wstring& value) -> uint32_t {
        if (value == L"env") return 0;
        if (value == L"llpc") return 1;
        if (value == L"aco") return 2;
        if (value == L"aco-strict") return 3;
        throw std::runtime_error("Compiler must be env, llpc, aco, or aco-strict");
    };

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        Require(i + 1 < argc, "Missing argument value");
        if (arg == L"--runtime") runtimePath = argv[++i];
        else if (arg == L"--baseline-runtime") baselinePath = argv[++i];
        else if (arg == L"--baseline-compiler") baselineCompiler = parseCompiler(argv[++i]);
        else if (arg == L"--candidate-compiler") candidateCompiler = parseCompiler(argv[++i]);
        else if (arg == L"--data-dir") dataDir = argv[++i];
        else if (arg == L"--baseline-data-dir") baselineDataDir = argv[++i];
        else if (arg == L"--dlssd") dlssdPath = argv[++i];
        else if (arg == L"--out") outDir = argv[++i];
        else if (arg == L"--rw") rw = std::stoul(argv[++i]);
        else if (arg == L"--rh") rh = std::stoul(argv[++i]);
        else if (arg == L"--ow") ow = std::stoul(argv[++i]);
        else if (arg == L"--oh") oh = std::stoul(argv[++i]);
        else if (arg == L"--warmup") warmup = std::stoul(argv[++i]);
        else if (arg == L"--frames") steady = std::stoul(argv[++i]);
        else if (arg == L"--drs-frames") drs = std::stoul(argv[++i]);
        else if (arg == L"--backend") backendStr = argv[++i];
        else if (arg == L"--fast-mode") fastMode = std::stol(argv[++i]);
        else if (arg == L"--optional-guides") optionalGuides = std::stol(argv[++i]) != 0;
        else if (arg == L"--parity-all-frames") parityAllFrames = std::stol(argv[++i]) != 0;
        else if (arg == L"--trace-frame") traceFrame = std::stol(argv[++i]);
        else if (arg == L"--align-engine-phase") alignPhase = std::stol(argv[++i]) != 0;
        else throw std::runtime_error("Unknown option");
    }
    if (baselineDataDir.empty()) baselineDataDir = dataDir;

    Require(!dlssdPath.empty() && fs::exists(dlssdPath), "Explicit --dlssd <path to nvngx_dlssd.dll 310.9.1.0> is required");
    Require(fs::exists(runtimePath), "Candidate runtime module not found: " + runtimePath.string());
    fs::create_directories(outDir);

    std::printf("=== OpenRR Offline Benchmark (%ux%u -> %ux%u, warmup=%u, steady=%u, drs=%u, fast_mode=%d, opt_guides=%d) ===\n",
                rw, rh, ow, oh, warmup, steady, drs, fastMode, optionalGuides ? 1 : 0);

    BenchResult baseRes {};
    bool hasBase = !baselinePath.empty();
    if (hasBase)
    {
        Require(fs::exists(baselinePath), "Baseline runtime module not found: " + baselinePath.string());
        baseRes = RunRuntimeBench("baseline", baselinePath, baselineDataDir, dlssdPath, outDir, rw, rh, ow, oh, warmup, steady, drs, backendStr, fastMode, optionalGuides, baselineCompiler, parityAllFrames, traceFrame, alignPhase);
        std::printf("[baseline] steady: mean=%.3f ms, median=%.3f ms, p95=%.3f ms, max=%.3f ms | DRS: mean=%.3f ms, median=%.3f ms, p95=%.3f ms, max=%.3f ms | VRAM: feature=%.1f MB (DRS %.1f MB), proc_delta=%.1f MB\n",
                    baseRes.steadyGpuMs.mean, baseRes.steadyGpuMs.median, baseRes.steadyGpuMs.p95, baseRes.steadyGpuMs.max,
                    baseRes.drsGpuMs.mean, baseRes.drsGpuMs.median, baseRes.drsGpuMs.p95, baseRes.drsGpuMs.max,
                    baseRes.featureSelfMbSteady, baseRes.featureSelfMbDrs, baseRes.processVramDeltaMbSteady);
    }

    BenchResult candRes = RunRuntimeBench("candidate", runtimePath, dataDir, dlssdPath, outDir, rw, rh, ow, oh, warmup, steady, drs, backendStr, fastMode, optionalGuides, candidateCompiler, parityAllFrames, traceFrame, alignPhase);
    std::printf("[candidate] steady: mean=%.3f ms, median=%.3f ms, p95=%.3f ms, max=%.3f ms | DRS: mean=%.3f ms, median=%.3f ms, p95=%.3f ms, max=%.3f ms | VRAM: feature=%.1f MB (DRS %.1f MB), proc_delta=%.1f MB\n",
                candRes.steadyGpuMs.mean, candRes.steadyGpuMs.median, candRes.steadyGpuMs.p95, candRes.steadyGpuMs.max,
                candRes.drsGpuMs.mean, candRes.drsGpuMs.median, candRes.drsGpuMs.p95, candRes.drsGpuMs.max,
                candRes.featureSelfMbSteady, candRes.featureSelfMbDrs, candRes.processVramDeltaMbSteady);

    bool parityPassed = true;
    std::string parityFailure;
    std::ostringstream parityJson;
    if (hasBase)
    {
        auto isFiniteHalf = [](uint16_t h) -> bool {
            return ((h >> 10) & 31u) != 31u;
        };
        auto halfToFloat = [](uint16_t h) -> double {
            uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
            double v = e == 0 ? std::ldexp(double(m), -24) : e == 31 ? std::numeric_limits<double>::quiet_NaN() : std::ldexp(double(m | 1024u), int(e) - 25);
            return s ? -v : v;
        };
        auto checkParity = [&](const std::vector<uint16_t>& aVec, const std::vector<uint16_t>& bVec, const char* phase) {
            Require(aVec.size() == bVec.size() && !aVec.empty(), std::string(phase) + " output size mismatch");
            uint32_t maxUlpDiff = 0;
            size_t diffCount = 0, baseNonFinite = 0, candNonFinite = 0;
            double seClamped = 0.0, seTm = 0.0;
            size_t rgbCount = 0;
            std::vector<float> absErrs;
            absErrs.reserve((bVec.size() * 3) / 4);
            for (size_t i = 0; i < bVec.size(); ++i)
            {
                uint16_t a = aVec[i], b = bVec[i];
                if (!isFiniteHalf(a)) ++baseNonFinite;
                if (!isFiniteHalf(b)) ++candNonFinite;
                if (a != b)
                {
                    ++diffCount;
                    uint32_t d = a > b ? a - b : b - a;
                    maxUlpDiff = std::max(maxUlpDiff, d);
                }
                if ((i & 3u) != 3u && isFiniteHalf(a) && isFiniteHalf(b))
                {
                    double fa = halfToFloat(a), fb = halfToFloat(b);
                    double ca = std::clamp(fa, 0.0, 1.0), cb = std::clamp(fb, 0.0, 1.0);
                    seClamped += (ca - cb) * (ca - cb);
                    double ta = std::max(fa, 0.0) / (1.0 + std::max(fa, 0.0));
                    double tb = std::max(fb, 0.0) / (1.0 + std::max(fb, 0.0));
                    seTm += (ta - tb) * (ta - tb);
                    absErrs.push_back(static_cast<float>(std::abs(fa - fb)));
                    ++rgbCount;
                }
            }
            std::sort(absErrs.begin(), absErrs.end());
            const double p99AbsErr = absErrs.empty() ? 0.0 : double(absErrs[size_t(absErrs.size() * 99 / 100)]);
            const double maxAbsErr = absErrs.empty() ? 0.0 : double(absErrs.back());
            const double exactPct = 100.0 * double(bVec.size() - diffCount) / double(bVec.size());
            const double psnr01 = (seClamped > 0.0 && rgbCount > 0) ? 10.0 * std::log10(1.0 / (seClamped / double(rgbCount))) : 999.0;
            const double psnrTm = (seTm > 0.0 && rgbCount > 0) ? 10.0 * std::log10(1.0 / (seTm / double(rgbCount))) : 999.0;
            std::printf("[parity] %s output pixels (%zu half-floats, non_finite=%zu): exact=%.6f%% (differing=%zu), max_ulp_diff=%u, PSNR_01=%.2f dB, PSNR_tonemap=%.2f dB, p99_abs_err=%.6f, max_abs_err=%.6f\n",
                        phase, bVec.size(), candNonFinite, exactPct, diffCount, maxUlpDiff, psnr01, psnrTm, p99AbsErr, maxAbsErr);
            const bool passed = baseNonFinite == 0 && candNonFinite == 0 &&
                (fastMode == 0 ? maxUlpDiff == 0 : psnr01 >= 54.0 && psnrTm >= 58.0 && p99AbsErr <= 0.006);
            if (!passed) {
                parityPassed = false;
                parityFailure += std::string(phase) + (fastMode == 0 ? " Legacy exact parity failed; " : " Fast quality gate failed; ");
            }
            if (parityJson.tellp() > 0) parityJson << ",\n";
            parityJson << "    \"" << phase << "\": {\"passed\": " << (passed ? "true" : "false")
                << ", \"baseline_non_finite\": " << baseNonFinite << ", \"candidate_non_finite\": " << candNonFinite
                << ", \"exact_percent\": " << exactPct << ", \"max_ulp_diff\": " << maxUlpDiff
                << ", \"psnr_01\": " << psnr01 << ", \"psnr_tonemap\": " << psnrTm
                << ", \"p99_abs_err\": " << p99AbsErr << ", \"max_abs_err\": " << maxAbsErr << "}";
        };
        checkParity(baseRes.steadyPixels, candRes.steadyPixels, "steady");
        checkParity(baseRes.drsPixels, candRes.drsPixels, "drs");
        Require(baseRes.frameOutputs.size() == candRes.frameOutputs.size(), "Per-frame parity count mismatch");
        for (size_t i=0;i<baseRes.frameOutputs.size();++i) {
            auto readPixels=[](const fs::path& path){
                std::ifstream file(path,std::ios::binary|std::ios::ate);
                Require(bool(file),"Cannot read parity output");auto n=file.tellg();
                Require(n>0 && n%2==0,"Invalid parity output size");
                std::vector<uint16_t> pixels(size_t(n)/2);file.seekg(0);
                file.read(reinterpret_cast<char*>(pixels.data()),n);Require(bool(file),"Truncated parity output");return pixels;
            };
            const auto phase=(i<steady?"steady_frame_":"drs_frame_")+std::to_string(i<steady?i:i-steady);
            checkParity(readPixels(baseRes.frameOutputs[i]),readPixels(candRes.frameOutputs[i]),phase.c_str());
        }
    }

    std::ofstream summary(outDir / "summary.json");
    summary << "{\n"
            << "  \"resolution\": \"" << rw << "x" << rh << "->" << ow << "x" << oh << "\",\n"
            << "  \"baseline_compiler\": " << baselineCompiler << ",\n"
            << "  \"candidate_compiler\": " << candidateCompiler << ",\n"
            << "  \"fast_mode\": " << fastMode << ",\n"
            << "  \"parity_coverage\": \"" << (parityAllFrames?"all_measured_frames":"phase_last_frame") << "\",\n"
            << "  \"warmup\": " << warmup << ", \"frames\": " << steady << ", \"drs_frames\": " << drs << ",\n"
            << "  \"parity_passed\": " << (parityPassed ? "true" : "false") << ",\n"
            << "  \"parity\": {\n" << parityJson.str() << "\n  },\n"
            << "  \"baseline\": {\"steady_median_ms\": " << baseRes.steadyGpuMs.median
            << ", \"steady_p95_ms\": " << baseRes.steadyGpuMs.p95 << ", \"drs_median_ms\": " << baseRes.drsGpuMs.median << "},\n"
            << "  \"candidate\": {\n"
            << "    \"steady_mean_ms\": " << candRes.steadyGpuMs.mean << ",\n"
            << "    \"steady_median_ms\": " << candRes.steadyGpuMs.median << ",\n"
            << "    \"steady_p95_ms\": " << candRes.steadyGpuMs.p95 << ",\n"
            << "    \"drs_mean_ms\": " << candRes.drsGpuMs.mean << ",\n"
            << "    \"drs_median_ms\": " << candRes.drsGpuMs.median << ",\n"
            << "    \"drs_p95_ms\": " << candRes.drsGpuMs.p95 << ",\n"
            << "    \"drs_max_ms\": " << candRes.drsGpuMs.max << ",\n"
            << "    \"feature_vram_mb\": " << candRes.featureSelfMbSteady << ",\n"
            << "    \"process_vram_delta_mb\": " << candRes.processVramDeltaMbSteady << "\n"
            << "  }\n"
            << "}\n";
    Require(parityPassed, parityFailure);
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: openrr_bench: %s\n", e.what());
    return 1;
}
