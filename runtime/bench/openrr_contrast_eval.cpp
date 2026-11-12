// SPDX-License-Identifier: MIT
// OpenRR vs RR_Contrast offline parity evaluator (anchor #2: RTX 5090 nvngx_dlssd 310.9.1.0).
//
// Synthesizes the exact rr_contrast_v1 / rr_contrast_v2_motion fixture frames on the CPU
// (rr_ref_scenes.hpp, included read-only from the RR_Contrast checkout), gates every hashed
// frame input against cases.json, drives openrr-amd.asi through OnrRrApi4 on the D3D12
// Direct path (polling ONR_RR_STATE_READY before the case's frame 0), and compares the
// saved key frames in memory against the archived RTX 5090 outputs. No GB-scale dumps.
//
// The JSON parsing is a small purpose-built reader; the D3D12 plumbing mirrors openrr_bench.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#include "opennr_rr_api.h"
#include "rr_ref_scenes.hpp"   // -I <RR_Contrast>/include (read-only checkout)

namespace fs = std::filesystem;

static void Fail(const std::string& what) { throw std::runtime_error(what); }
static void Require(bool ok, const std::string& what) { if (!ok) Fail(what); }
static void Check(HRESULT hr, const char* what) { if (FAILED(hr)) Fail(std::string(what) + " failed"); }

// ---------------------------------------------------------------- minimal JSON
struct JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
struct JsonValue
{
    enum class Kind { Null, Bool, Num, Str, Arr, Obj } kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    JsonObject obj;

    const JsonValue* Find(const char* key) const
    {
        if (kind != Kind::Obj) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    double GetNum(const char* key, double dflt) const { const JsonValue* v = Find(key); return v && v->kind == Kind::Num ? v->num : dflt; }
    std::string GetStr(const char* key, const std::string& dflt = "") const { const JsonValue* v = Find(key); return v && v->kind == Kind::Str ? v->str : dflt; }
    bool GetBool(const char* key, bool dflt) const { const JsonValue* v = Find(key); return v && v->kind == Kind::Bool ? v->b : dflt; }
};

struct JsonParser
{
    std::string text;
    const char* p = nullptr;
    explicit JsonParser(const std::string& t) : text(t), p(text.c_str()) {}
    void SkipWs() { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p; }
    JsonValue Parse()
    {
        SkipWs();
        JsonValue v;
        if (*p == '{')
        {
            ++p; v.kind = JsonValue::Kind::Obj;
            SkipWs();
            if (*p == '}') { ++p; return v; }
            while (true)
            {
                SkipWs();
                std::string key = ParseString();
                SkipWs(); Require(*p == ':', "json ':' expected"); ++p;
                v.obj[key] = Parse();
                SkipWs();
                if (*p == ',') { ++p; continue; }
                Require(*p == '}', "json '}' expected"); ++p; break;
            }
        }
        else if (*p == '[')
        {
            ++p; v.kind = JsonValue::Kind::Arr;
            SkipWs();
            if (*p == ']') { ++p; return v; }
            while (true)
            {
                v.arr.push_back(Parse());
                SkipWs();
                if (*p == ',') { ++p; continue; }
                Require(*p == ']', "json ']' expected"); ++p; break;
            }
        }
        else if (*p == '"') v.str = ParseString(), v.kind = JsonValue::Kind::Str;
        else if (std::strncmp(p, "true", 4) == 0) { p += 4; v.kind = JsonValue::Kind::Bool; v.b = true; }
        else if (std::strncmp(p, "false", 5) == 0) { p += 5; v.kind = JsonValue::Kind::Bool; v.b = false; }
        else if (std::strncmp(p, "null", 4) == 0) { p += 4; }
        else
        {
            v.kind = JsonValue::Kind::Num;
            char* end = nullptr;
            v.num = std::strtod(p, &end);
            Require(end != p, std::string("json number expected near: ") + std::string(p, std::min<size_t>(48, std::strlen(p))));
            p = end;
        }
        return v;
    }
    std::string ParseString()
    {
        Require(*p == '"', "json string expected");
        ++p;
        std::string out;
        while (*p && *p != '"')
        {
            if (*p == '\\' && p[1])
            {
                ++p;
                switch (*p)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u':
                {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) { ++p; Require(*p, "json \\u cut"); cp = cp * 16u + unsigned(std::isxdigit(static_cast<unsigned char>(*p)) ? (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10) : 0); }
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                    break;
                }
                default: out += *p; break;
                }
                ++p;
            }
            else out += *p++;
        }
        Require(*p == '"', "json string unterminated");
        ++p;
        return out;
    }
};

static JsonValue LoadJson(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    Require(bool(f), "cannot open " + path.string());
    std::ostringstream ss; ss << f.rdbuf();
    JsonParser parser(ss.str());
    JsonValue v = parser.Parse();
    return v;
}

// ---------------------------------------------------------------- suite cases
struct SuiteCase
{
    std::string id, pass;
    rr_ref::SceneId scene = rr_ref::SceneId::S0_BenchParity;
    uint32_t guide_mask = 0;
    uint32_t alloc_w = 0, alloc_h = 0, steady_w = 0, steady_h = 0, drs_w = 0, drs_h = 0;
    uint32_t out_w = 0, out_h = 0;
    int32_t perf_quality = 2;
    uint32_t preset_hint = 6;
    uint32_t warmup = 0, steady_frames = 0, drs_frames = 0;
    std::vector<uint32_t> save_frames;
    std::map<std::string, std::string> expected_input_sha256;   // key -> hash (f000_input_sha256, ...)
    // v2_motion modifiers
    int32_t create_flags = 0x4B;
    uint32_t in_base_x = 0, in_base_y = 0;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    int64_t mv_tex_scale_num = 1;
    int32_t mv_jitter_delta_sign = 0;
    bool manual_exposure_step = false, manual_exposure_tex_step = false;
    bool abi_not_exposed_yet = false;   // inputs the OnrRrFrame ABI cannot express (exposure probes)
    std::string abi_note;
};

static SuiteCase CaseFromJson(const JsonValue& j)
{
    SuiteCase c;
    c.id = j.GetStr("id");
    c.pass = j.GetStr("pass");
    c.scene = rr_ref::ParseSceneId(j.GetStr("scene"));
    c.guide_mask = uint32_t(j.GetNum("guide_mask", 0));
    const JsonValue* a = j.Find("alloc_render"); c.alloc_w = uint32_t(a->arr[0].num); c.alloc_h = uint32_t(a->arr[1].num);
    const JsonValue* s = j.Find("steady_render"); c.steady_w = uint32_t(s->arr[0].num); c.steady_h = uint32_t(s->arr[1].num);
    const JsonValue* d = j.Find("drs_render");
    if (d && d->kind == JsonValue::Kind::Arr && d->arr.size() == 2) { c.drs_w = uint32_t(d->arr[0].num); c.drs_h = uint32_t(d->arr[1].num); }
    const JsonValue* o = j.Find("output"); c.out_w = uint32_t(o->arr[0].num); c.out_h = uint32_t(o->arr[1].num);
    c.perf_quality = int32_t(j.GetNum("perf_quality", 2));
    c.preset_hint = uint32_t(j.GetNum("preset_hint", 6));
    c.warmup = uint32_t(j.GetNum("warmup_frames", 0));
    c.steady_frames = uint32_t(j.GetNum("steady_frames", 0));
    c.drs_frames = uint32_t(j.GetNum("drs_frames", 0));
    const JsonValue* sf = j.Find("save_frames");
    if (sf) for (const auto& v : sf->arr) c.save_frames.push_back(uint32_t(v.num));
    for (const auto& [k, v] : j.obj)
        if (k.size() > 13 && k.compare(k.size() - 13, 13, "_input_sha256") == 0 && v.kind == JsonValue::Kind::Str)
            c.expected_input_sha256[k] = v.str;
    c.create_flags = int32_t(j.GetNum("create_flags", 0x4B));
    const JsonValue* ib = j.Find("in_subrect_base");
    if (ib && ib->kind == JsonValue::Kind::Arr) { c.in_base_x = uint32_t(ib->arr[0].num); c.in_base_y = uint32_t(ib->arr[1].num); }
    const JsonValue* ms = j.Find("mv_scale");
    if (ms && ms->kind == JsonValue::Kind::Arr) { c.mv_scale_x = float(ms->arr[0].num); c.mv_scale_y = float(ms->arr[1].num); }
    c.mv_tex_scale_num = int64_t(j.GetNum("mv_tex_scale_num", 1));
    c.mv_jitter_delta_sign = int32_t(j.GetNum("mv_jitter_delta_sign", 0));
    c.manual_exposure_step = j.GetBool("manual_exposure_step", false);
    // cases_v2_motion.json serializes only manual_exposure_step; the tex variant is implied by
    // kGuideExposureTex in the mask (the runner's writer omits the field, its cases set it).
    c.manual_exposure_tex_step = j.GetBool("manual_exposure_tex_step", (c.guide_mask & (1u << 11)) != 0u);
    if (c.manual_exposure_step)
    {
        c.abi_not_exposed_yet = true;
        c.abi_note = "Pre.Exposure / Exposure.Scale scalars have no OnrRrFrame field";
    }
    if (c.manual_exposure_tex_step)
    {
        c.abi_not_exposed_yet = true;
        c.abi_note = "1x1 ExposureTexture has no OnrRrFrame field";
    }
    return c;
}

// ---------------------------------------------------------------- D3D12 plumbing
struct GpuTex
{
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    void Create(ID3D12Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav, D3D12_RESOURCE_STATES init)
    {
        w = width; h = height; fmt = format; state = init;
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = width; rd.Height = height;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init, nullptr, IID_PPV_ARGS(&res)), "CreateCommittedResource");
    }
};

// host command-list lifecycle notifications (slot retirement), resolved once at module load
static void (*g_notifyListReset)(void*, void*) = nullptr;
static void (*g_notifyAllocReset)(void*) = nullptr;

constexpr D3D12_RESOURCE_STATES kReadState = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

struct Dev
{
    ComPtr<IDXGIAdapter3> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;

    void Init()
    {
        ComPtr<IDXGIFactory6> fac;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> ad1;
        for (UINT i = 0; fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad1)) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc {};
            ad1->GetDesc1(&desc);
            if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { ad1.As(&adapter); break; }
        }
        Require(adapter != nullptr, "no AMD adapter");
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
        Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        Check(list->Close(), "close initial");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    }
    void Flush(const char* what)
    {
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++fenceValue), "signal");
        WaitFor(what);
    }
    void WaitFor(const char* what)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(fence->SetEventOnCompletion(fenceValue, ev), "SetEventOnCompletion");
        Require(WaitForSingleObject(ev, 120000) == WAIT_OBJECT_0, std::string("fence timeout: ") + what);
        CloseHandle(ev);
    }
};

// Upload a batch of (texture, bytes) in one command list execution. bpp is the texel size of every
// texture in the batch (mixed batches are split by the caller).
static void UploadTextures(Dev& dev, const std::vector<std::pair<GpuTex*, const void*>>& items, uint32_t bpp)
{
    if (items.empty()) return;
    ID3D12Device* d = dev.device.Get();
    struct Foot { GpuTex* t; const void* data; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 off; };
    std::vector<Foot> foots;
    UINT64 total = 0;
    for (auto& [t, data] : items)
    {
        D3D12_RESOURCE_DESC desc = t->res->GetDesc();
        Foot f { t, data, {}, 0 };
        UINT64 thisTotal = 0;
        d->GetCopyableFootprints(&desc, 0, 1, 0, &f.fp, nullptr, nullptr, &thisTotal);
        f.off = total;
        total += thisTotal;
        foots.push_back(f);
    }
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> up;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)), "upload buffer");
    uint8_t* m = nullptr;
    Check(up->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map upload");
    for (const auto& f : foots)
    {
        const uint8_t* src = static_cast<const uint8_t*>(f.data);
        const uint32_t rowBytes = f.t->w * bpp;
        for (uint32_t y = 0; y < f.t->h; ++y)
            std::memcpy(m + f.off + size_t(y) * f.fp.Footprint.RowPitch, src + size_t(y) * rowBytes, rowBytes);
    }
    up->Unmap(0, nullptr);

    Check(dev.alloc->Reset(), "reset alloc");
    if (g_notifyAllocReset) g_notifyAllocReset(dev.alloc.Get());
    Check(dev.list->Reset(dev.alloc.Get(), nullptr), "reset list");
    if (g_notifyListReset) g_notifyListReset(dev.list.Get(), dev.alloc.Get());
    for (const auto& f : foots)
    {
        if (f.t->state != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { f.t->res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, f.t->state, D3D12_RESOURCE_STATE_COPY_DEST };
            dev.list->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst { f.t->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
        D3D12_TEXTURE_COPY_LOCATION src { up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
        src.PlacedFootprint = f.fp;
        src.PlacedFootprint.Offset = f.off;
        dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { f.t->res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, kReadState };
        dev.list->ResourceBarrier(1, &b);
        f.t->state = kReadState;
    }
    Check(dev.list->Close(), "close upload");
    dev.Flush("upload");
}

static std::vector<uint16_t> ReadbackOutput(Dev& dev, GpuTex& tex)
{
    ID3D12Device* d = dev.device.Get();
    D3D12_RESOURCE_DESC desc = tex.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    d->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "readback buffer");
    Check(dev.alloc->Reset(), "reset alloc");
    if (g_notifyAllocReset) g_notifyAllocReset(dev.alloc.Get());
    Check(dev.list->Reset(dev.alloc.Get(), nullptr), "reset list");
    if (g_notifyListReset) g_notifyListReset(dev.list.Get(), dev.alloc.Get());
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, tex.state, D3D12_RESOURCE_STATE_COPY_SOURCE };
    dev.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src { tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    dev.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    dev.list->ResourceBarrier(1, &b);
    Check(dev.list->Close(), "close readback");
    dev.Flush("readback");
    uint8_t* mapped = nullptr;
    Check(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map readback");
    std::vector<uint16_t> out(size_t(tex.w) * tex.h * 4);
    for (uint32_t y = 0; y < tex.h; ++y)
        std::memcpy(out.data() + size_t(y) * tex.w * 4, mapped + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(tex.w) * 8);
    D3D12_RANGE empty {};
    rb->Unmap(0, &empty);
    return out;
}

// ---------------------------------------------------------------- metrics
struct FrameMetrics
{
    size_t diff_halfs = 0;
    uint32_t max_ulp = 0;
    double psnr01_db = 999.0;
    double psnr_reinhard_db = 999.0;
    double max_abs = 0.0, mean_abs = 0.0;
    double rgb_rmse = 0.0;   // unclipped linear HDR RMSE (5090-report compatible)
    double exact_half_pct = 100.0;
    size_t non_finite = 0;
};

static FrameMetrics CompareRgba16f(const std::vector<uint16_t>& ref, const std::vector<uint16_t>& cur)
{
    FrameMetrics m {};
    if (ref.size() != cur.size() || ref.empty())
    {
        m.psnr01_db = m.psnr_reinhard_db = -1.0;
        return m;
    }
    double se01 = 0.0, seR = 0.0, seLinear = 0.0, sumAbs = 0.0;
    size_t rgb_n = 0;
    for (size_t i = 0; i < cur.size(); ++i)
    {
        const uint16_t a = ref[i], b = cur[i];
        if (!rr_ref::IsFiniteHalf(a) || !rr_ref::IsFiniteHalf(b)) ++m.non_finite;
        if (a != b)
        {
            ++m.diff_halfs;
            m.max_ulp = std::max(m.max_ulp, uint32_t(a > b ? a - b : b - a));
        }
        if ((i & 3u) != 3u && rr_ref::IsFiniteHalf(a) && rr_ref::IsFiniteHalf(b))
        {
            const double da = rr_ref::HalfToDouble(a), db = rr_ref::HalfToDouble(b);
            const double c01a = std::clamp(da, 0.0, 1.0), c01b = std::clamp(db, 0.0, 1.0);
            se01 += (c01a - c01b) * (c01a - c01b);
            const double ra = da / (1.0 + std::abs(da)), rb = db / (1.0 + std::abs(db));
            seR += (ra - rb) * (ra - rb);
            const double ad = std::abs(da - db);
            seLinear += ad * ad;
            m.max_abs = std::max(m.max_abs, ad);
            sumAbs += ad;
            ++rgb_n;
        }
    }
    if (rgb_n)
    {
        if (se01 > 0.0) m.psnr01_db = 10.0 * std::log10(1.0 / (se01 / double(rgb_n)));
        if (seR > 0.0) m.psnr_reinhard_db = 10.0 * std::log10(1.0 / (seR / double(rgb_n)));
        m.mean_abs = sumAbs / double(rgb_n);
        m.rgb_rmse = std::sqrt(seLinear / double(rgb_n));
    }
    m.exact_half_pct = 100.0 * double(cur.size() - m.diff_halfs) / double(cur.size());
    return m;
}

static bool TryLoadRawRgba16f(const fs::path& p, size_t halfs, std::vector<uint16_t>& out)
{
    std::error_code ec;
    if (!fs::exists(p, ec) || fs::file_size(p, ec) != halfs * sizeof(uint16_t)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize(halfs);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(halfs * sizeof(uint16_t)));
    return bool(f);
}

// ---------------------------------------------------------------- the evaluator
struct Config
{
    std::string name;
    int32_t fastMode = 0;
    uint32_t compiler = 0;
};

struct Args
{
    fs::path asi, dataDir, dlssd, refDir, outDir, casesJson;
    std::string suite = "v1", config = "fast-aco";
    std::vector<std::string> onlyPasses, onlyCases;
    bool noGpu = false;
};

struct HashGateRow
{
    std::string caseId, key;
    std::string expected, computed;
    bool pass = false;
};

// Map the hash key to the frame it pins: f000 / f015 / drs_f064_reset0 / drs_f072_reset1 / v2 probe keys.
static uint32_t FrameForHashKey(const std::string& key)
{
    if (key.rfind("f", 0) == 0 && key.find("_input_sha256") != std::string::npos)
    {
        const std::string num = key.substr(1, key.size() - 1 - strlen("_input_sha256"));
        if (!num.empty() && std::all_of(num.begin(), num.end(), ::isdigit)) return uint32_t(std::stoul(num));
    }
    if (key == "drs_f064_reset0_input_sha256") return 64u;
    if (key == "drs_f072_reset1_input_sha256") return 72u;
    return UINT32_MAX;
}

int RunMain(const Args& args)
{
    const rr_ref::FixtureSuite suite = args.suite == "v2_motion" ? rr_ref::FixtureSuite::V2_Motion : rr_ref::FixtureSuite::V1;
    rr_ref::RunSelfTests();
    const JsonValue casesRoot = LoadJson(args.casesJson);
    Require(casesRoot.GetStr("fixture_version") == rr_ref::FixtureVersionForSuite(suite),
            "cases file fixture_version mismatch: " + casesRoot.GetStr("fixture_version"));
    std::vector<SuiteCase> cases;
    for (const auto& cj : casesRoot.Find("cases")->arr) cases.push_back(CaseFromJson(cj));
    if (!args.onlyCases.empty())
    {
        std::vector<SuiteCase> keep;
        for (const auto& c : cases)
            if (std::find(args.onlyCases.begin(), args.onlyCases.end(), c.id) != args.onlyCases.end()) keep.push_back(c);
        cases = keep;
    }
    else if (!args.onlyPasses.empty())
    {
        std::vector<SuiteCase> keep;
        for (const auto& c : cases)
            if (std::find(args.onlyPasses.begin(), args.onlyPasses.end(), c.pass) != args.onlyPasses.end()) keep.push_back(c);
        cases = keep;
    }
    Require(!cases.empty(), "no cases selected");

    Config cfg { "legacy", 0, 1 };
    if (args.config == "legacy") cfg = { "legacy", 0, 1 };
    else if (args.config == "fast-llpc") cfg = { "fast-llpc", 1, 1 };
    else if (args.config == "fast-aco") cfg = { "fast-aco", 1, 0 };
    else Fail("unknown --config " + args.config);

    fs::create_directories(args.outDir);
    std::ofstream csv(args.outDir / "per_frame.csv", std::ios::binary);
    csv << "case_id,frame,phase,rw,rh,reset,hash_gate,ref_file,psnr01_db,psnr_reinhard_db,rgb_rmse,max_abs,mean_abs,max_ulp,exact_half_pct,non_finite,diff_halfs\n";
    std::ofstream gatesCsv(args.outDir / "hash_gates.csv", std::ios::binary);
    gatesCsv << "case_id,key,expected,computed,pass\n";
    std::ostringstream summary;
    summary << "{\n  \"suite\": \"" << rr_ref::FixtureVersionForSuite(suite) << "\",\n  \"config\": \"" << cfg.name << "\",\n  \"cases\": [\n";

    Dev dev;
    if (!args.noGpu) dev.Init();

    HMODULE mod = nullptr;
    PFN_OnrRrGetApi getApi = nullptr;
    decltype(&OnrRrSetFastMode) setFastMode = nullptr;
    decltype(&OnrRrGetFastMode) getFastMode = nullptr;
    if (!args.noGpu)
    {
        mod = LoadLibraryExW(fs::absolute(args.asi).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Require(mod, "cannot load " + args.asi.string());
        if (auto vkLoad = reinterpret_cast<void(*)()>(GetProcAddress(mod, "rr_vk_load"))) vkLoad();
        getApi = reinterpret_cast<PFN_OnrRrGetApi>(GetProcAddress(mod, "OnrRrGetApi"));
        Require(getApi, "missing OnrRrGetApi");
        setFastMode = reinterpret_cast<decltype(&OnrRrSetFastMode)>(GetProcAddress(mod, "OnrRrSetFastMode"));
        getFastMode = reinterpret_cast<decltype(&OnrRrGetFastMode)>(GetProcAddress(mod, "OnrRrGetFastMode"));
        g_notifyListReset = reinterpret_cast<void(*)(void*, void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12ListReset"));
        g_notifyAllocReset = reinterpret_cast<void(*)(void*)>(GetProcAddress(mod, "OnrRrNotifyD3d12AllocatorReset"));
    }

    size_t written = 0;
    std::map<std::string, FrameMetrics> probeF015;             // OpenRR self f015 metrics vs P00 / pair
    std::map<std::string, std::vector<uint16_t>> probeF015Px;  // own pixels for pair comparisons
    std::string p00F015Sha;

    for (const SuiteCase& c : cases)
    {
        std::printf("[%s] %s ... %s\n", cfg.name.c_str(), c.id.c_str(), c.abi_not_exposed_yet ? c.abi_note.c_str() : "");
        const bool gpuRun = !args.noGpu && !c.abi_not_exposed_yet;

        // CPU side: buffers + hash gate over every pinned frame.
        rr_ref::CpuFrameBuffers bufs;
        bufs.Allocate(c.alloc_w, c.alloc_h, c.out_w, c.out_h, c.guide_mask | 0xFFFu); // allocate every guide slot the mask may use
        if (c.scene == rr_ref::SceneId::S0_BenchParity) rr_ref::PopulateStaticS0(bufs);
        std::map<uint32_t, rr_ref::FrameScalarParams> pinned;
        for (const auto& [key, expected] : c.expected_input_sha256)
        {
            const bool isDrsKey = key.rfind("drs_f", 0) == 0;
            const uint32_t f = isDrsKey
                ? uint32_t(std::stoul(key.substr(5, 3)))
                : FrameForHashKey(key);
            Require(f != UINT32_MAX, "unknown hash key " + key + " in " + c.id);
            uint32_t rw = c.steady_w, rh = c.steady_h;
            int32_t reset = 0;
            if (isDrsKey)
            {
                // fixture semantics: the drs keys pin content at a fixed frame index with the DRS
                // size, independent of this case's steady length (T-cases keep the same key)
                rw = c.drs_w; rh = c.drs_h;
                reset = (key.find("_reset1_") != std::string::npos) ? 1 : 0;
            }
            else if (c.drs_frames && f >= c.steady_frames)
            {
                const uint32_t di = f - c.steady_frames;
                const bool lower = ((di / 4u) % 2u) == 0u;
                rw = lower ? c.drs_w : c.steady_w;
                rh = lower ? c.drs_h : c.steady_h;
                reset = (di >= 8u && (di % 4u) == 0u) ? 1 : 0;
            }
            if (f == 0 && !isDrsKey) reset = 1;
            rr_ref::V2FrameModifiers v2 {};
            v2.create_flags = c.create_flags; v2.in_base_x = c.in_base_x; v2.in_base_y = c.in_base_y;
            v2.mv_scale_x = c.mv_scale_x; v2.mv_scale_y = c.mv_scale_y;
            v2.mv_tex_scale_num = c.mv_tex_scale_num; v2.mv_jitter_delta_sign = c.mv_jitter_delta_sign;
            v2.manual_exposure_step = c.manual_exposure_step; v2.manual_exposure_tex_step = c.manual_exposure_tex_step;
            rr_ref::FrameScalarParams p;
            rr_ref::PrepareFrame(c.scene, c.guide_mask, f, rw, rh, c.out_w, c.out_h, reset, c.preset_hint, c.perf_quality, p, bufs, suite, v2);
            const std::string got = rr_ref::ComputeCanonicalFrameInputSha256(p, c.guide_mask, bufs, suite);
            gatesCsv << c.id << ',' << key << ',' << expected << ',' << got << ',' << (got == expected ? 1 : 0) << '\n';
            if (got != expected)
                Fail("input hash mismatch " + c.id + " " + key + ": expected " + expected + " got " + got);
            pinned[f] = p;
        }

        FrameMetrics f000VsP00, f015VsP00, f015VsPair, f015Vs5090;
        std::map<uint32_t, FrameMetrics> frameMetrics;   // vs 5090 ref
        std::vector<uint16_t> ownF015;
        std::string status = "ok";

        if (gpuRun)
        {
            OnrRrApi4 api {};
            Require(getApi(ONR_RR_ABI_VERSION_4, &api.v3.v2.v1) == ONR_RR_OK, "OnrRrGetApi(v4) failed");
            OnrRrCreateInfo ci {};
            ci.size = sizeof(ci);
            ci.mode = ONR_RR_MODE_FAST;
            ci.compiler = cfg.compiler;
            ci.d3d12_device = dev.device.Get();
            ci.max_render_width = c.alloc_w;
            ci.max_render_height = c.alloc_h;
            ci.output_width = c.out_w;
            ci.output_height = c.out_h;
            const std::wstring dlssdW = fs::absolute(args.dlssd).wstring();
            const std::wstring dataW = fs::absolute(args.dataDir).wstring();
            const std::wstring cacheW = fs::absolute(args.outDir / "cache").wstring();
            const std::wstring logW = fs::absolute(args.outDir / ("openrr_" + c.id + ".log")).wstring();
            ci.dlssd_path = dlssdW.c_str();
            ci.data_dir = dataW.c_str();
            ci.cache_dir = cacheW.c_str();
            ci.log_path = logW.c_str();
            ci.backend = ONR_RR_BACKEND_CHOICE_D3D12;
            void* ctx = nullptr;
            Require(api.v3.v2.v1.Create(&ci, &ctx) == ONR_RR_OK && ctx, "Create failed for " + c.id);
            if (setFastMode) Require(setFastMode(ctx, cfg.fastMode) == ONR_RR_OK, "SetFastMode failed");

            // Textures at the alloc extent; the active subrect moves with in_base (P14).
            GpuTex color, diff, spec, norm, rough, depth, mv, hit, output;
            GpuTex disocc, tlayer, topacity, tlmv, sss, cbt, specmv, resp, dof;
            color.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            norm.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            mv.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            diff.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
            spec.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
            depth.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            hit.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            output.Create(dev.device.Get(), c.out_w, c.out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t gm = c.guide_mask;
            if (gm & (1u << 0)) disocc.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & (1u << 1)) { tlayer.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST); topacity.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST); }
            if (gm & (1u << 2)) tlmv.Create(dev.device.Get(), c.out_w, c.out_h, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & ((1u << 3) | (1u << 8))) sss.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & ((1u << 3) | (1u << 9))) cbt.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & (1u << 4)) specmv.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & (1u << 5)) resp.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & (1u << 6)) dof.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            if (gm & (1u << 10)) rough.Create(dev.device.Get(), c.alloc_w, c.alloc_h, DXGI_FORMAT_R16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
            const uint32_t inBx = c.in_base_x, inBy = c.in_base_y;
            const bool omitHit = (gm & (1u << 7)) != 0u;
            const bool unpackedRough = (gm & (1u << 10)) != 0u;

            // one frame into the command list; the caller has uploaded matching texture content
            auto submitFrame = [&](const rr_ref::FrameScalarParams& p) {
                OnrRrFrame fr {};
                fr.size = sizeof(fr);
                fr.flags = ONR_RR_FRAME_HW_DEPTH | ONR_RR_FRAME_ROUGHNESS_PACKED
                         | ((c.create_flags & 0x08) ? ONR_RR_FRAME_DEPTH_INVERTED : 0u)
                         | ((c.create_flags & 0x04) ? ONR_RR_FRAME_MV_JITTERED : 0u)
                         | (p.reset ? ONR_RR_FRAME_RESET : 0u);
                if (unpackedRough) fr.flags &= ~ONR_RR_FRAME_ROUGHNESS_PACKED;
                fr.render_width = p.rw;
                fr.render_height = p.rh;
                fr.jitter_x = p.jitter_x; fr.jitter_y = p.jitter_y;
                fr.mv_scale_x = p.mv_scale_x; fr.mv_scale_y = p.mv_scale_y;
                std::memcpy(fr.world_to_view, p.world_to_view, sizeof(fr.world_to_view));
                std::memcpy(fr.view_to_clip, p.view_to_clip, sizeof(fr.view_to_clip));
                fr.color = { color.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.diffuse_albedo = { diff.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.specular_albedo = { spec.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.normals = { norm.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.depth = { depth.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.motion = { mv.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (!omitHit) fr.specular_hit_distance = { hit.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                fr.output = { output.res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0 };
                if (gm & (1u << 0)) fr.disocclusion_mask = { disocc.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (gm & (1u << 1))
                {
                    fr.transparency_layer = { tlayer.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                    fr.transparency_opacity = { topacity.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                }
                if (gm & (1u << 2)) fr.transparency_motion = { tlmv.res.Get(), kReadState, 0, 0 };
                if (gm & ((1u << 3) | (1u << 8))) fr.sss_guide = { sss.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (gm & ((1u << 3) | (1u << 9))) fr.color_before_transparency = { cbt.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (gm & (1u << 4)) fr.specular_motion = { specmv.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (gm & (1u << 5)) fr.responsivity_mask = { resp.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (gm & (1u << 6)) fr.depth_of_field_guide = { dof.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                if (unpackedRough) fr.roughness = { rough.res.Get(), kReadState, p.in_base_x, p.in_base_y };
                Check(dev.alloc->Reset(), "reset alloc");
                if (g_notifyAllocReset) g_notifyAllocReset(dev.alloc.Get());
                Check(dev.list->Reset(dev.alloc.Get(), nullptr), "reset list");
                if (g_notifyListReset) g_notifyListReset(dev.list.Get(), dev.alloc.Get());
                void* job = nullptr;
                Require(api.Record(ctx, &fr, dev.list.Get(), ONR_RR_EDIT_OFF, nullptr, &job) == ONR_RR_OK, "Record failed");
                Check(dev.list->Close(), "close record");
                dev.Flush("record frame");
            };
            auto uploadFrame = [&](const rr_ref::CpuFrameBuffers& b) {
                std::vector<std::pair<GpuTex*, const void*>> items8, items4, items2;
                items8.push_back({ &color, b.color_rgba16f.data() });
                items8.push_back({ &norm, b.normals_rgba16f.data() });
                items4.push_back({ &mv, b.mvec_rg16f.data() });
                items4.push_back({ &diff, b.diffuse_rgba8.data() });
                items4.push_back({ &spec, b.specular_rgba8.data() });
                items4.push_back({ &depth, b.depth_r32f.data() });
                if (!omitHit) items4.push_back({ &hit, b.hit_dist_r32f.data() });
                if (gm & (1u << 0)) items2.push_back({ &disocc, b.disocc_r16f.data() });
                if (gm & (1u << 1)) { items8.push_back({ &tlayer, b.transp_rgba16f.data() }); items8.push_back({ &topacity, b.transp_op_rgba16f.data() }); }
                if (gm & (1u << 2)) items4.push_back({ &tlmv, b.transp_mvec_rg16f.data() });
                if (gm & ((1u << 3) | (1u << 8))) items2.push_back({ &sss, b.sss_r16f.data() });
                if (gm & ((1u << 3) | (1u << 9))) items8.push_back({ &cbt, b.cbt_rgba16f.data() });
                if (gm & (1u << 4)) items4.push_back({ &specmv, b.spec_mvec_rg16f.data() });
                if (gm & (1u << 5)) items2.push_back({ &resp, b.resp_r16f.data() });
                if (gm & (1u << 6)) items2.push_back({ &dof, b.dof_r16f.data() });
                if (unpackedRough) items2.push_back({ &rough, b.rough_r16f.data() });
                UploadTextures(dev, items8, 8);
                UploadTextures(dev, items4, 4);
                UploadTextures(dev, items2, 2);
            };

            // Anchor-2 rule: the case's frame 0 must never fall into the BUILDING bilinear fallback.
            // The network (and, in fast mode, the fast kernels, which build on the first frame that asks)
            // needs priming frames, so submit reset frames at the steady size until READY + fast built;
            // the case's own f000 carries RESET and discards anything the priming left in the history.
            rr_ref::FrameScalarParams priming {};
            {
                rr_ref::V2FrameModifiers v2 {};
                v2.create_flags = c.create_flags; v2.in_base_x = c.in_base_x; v2.in_base_y = c.in_base_y;
                v2.mv_scale_x = c.mv_scale_x; v2.mv_scale_y = c.mv_scale_y;
                v2.mv_tex_scale_num = c.mv_tex_scale_num; v2.mv_jitter_delta_sign = c.mv_jitter_delta_sign;
                v2.manual_exposure_step = c.manual_exposure_step; v2.manual_exposure_tex_step = c.manual_exposure_tex_step;
                rr_ref::PrepareFrame(c.scene, c.guide_mask, 0u, c.steady_w, c.steady_h, c.out_w, c.out_h, 1, c.preset_hint, c.perf_quality, priming, bufs, suite, v2);
            }
            uploadFrame(bufs);
            const ULONGLONG deadline = GetTickCount64() + 240000;
            bool ready = false;
            while (GetTickCount64() < deadline && !ready)
            {
                OnrRrInfo info {};
                info.size = sizeof(info);
                Require(api.v3.v2.v1.GetInfo(ctx, &info) == ONR_RR_OK, "GetInfo");
                Require(info.state != ONR_RR_STATE_FAILED, std::string("runtime failed: ") + info.message);
                submitFrame(priming);
                const bool fastReady = !cfg.fastMode || (getFastMode && getFastMode(ctx) == 1);
                if (info.state == ONR_RR_STATE_READY && info.frames >= 1 && fastReady) ready = true;
                else Sleep(10);
            }
            Require(ready, "runtime did not reach READY (+fast built) for " + c.id);

            const bool staticScene = c.scene == rr_ref::SceneId::S0_BenchParity;
            const uint32_t totalFrames = c.steady_frames + c.drs_frames;
            for (uint32_t f = 0; f < totalFrames; ++f)
            {
                uint32_t rw = c.steady_w, rh = c.steady_h;
                int32_t reset = (f == 0) ? 1 : 0;
                if (c.drs_frames && f >= c.steady_frames)
                {
                    const uint32_t di = f - c.steady_frames;
                    const bool lower = ((di / 4u) % 2u) == 0u;
                    rw = lower ? c.drs_w : c.steady_w;
                    rh = lower ? c.drs_h : c.steady_h;
                    reset = (di >= 8u && (di % 4u) == 0u) ? 1 : 0;
                }
                rr_ref::V2FrameModifiers v2 {};
                v2.create_flags = c.create_flags; v2.in_base_x = c.in_base_x; v2.in_base_y = c.in_base_y;
                v2.mv_scale_x = c.mv_scale_x; v2.mv_scale_y = c.mv_scale_y;
                v2.mv_tex_scale_num = c.mv_tex_scale_num; v2.mv_jitter_delta_sign = c.mv_jitter_delta_sign;
                v2.manual_exposure_step = c.manual_exposure_step; v2.manual_exposure_tex_step = c.manual_exposure_tex_step;
                rr_ref::FrameScalarParams p;
                rr_ref::PrepareFrame(c.scene, c.guide_mask, f, rw, rh, c.out_w, c.out_h, reset, c.preset_hint, c.perf_quality, p, bufs, suite, v2);
                if (pinned.count(f)) Require(pinned[f].frame_index == p.frame_index, "pinned frame regen mismatch");

                if (!staticScene || f == 0) uploadFrame(bufs);
                submitFrame(p);

                const bool save = std::find(c.save_frames.begin(), c.save_frames.end(), f) != c.save_frames.end();
                if (save)
                {
                    auto px = ReadbackOutput(dev, output);
                    char refn[64];
                    if (c.pass == "probes") std::snprintf(refn, sizeof(refn), "f%03u.rgba16f", f);
                    else std::snprintf(refn, sizeof(refn), "f%03u_%ux%u_to_%ux%u.rgba16f", f, rw, rh, c.out_w, c.out_h);
                    const fs::path refFile = args.refDir.empty() ? fs::path()
                        : args.refDir / (c.pass == "probes" ? "05_probes" : (c.pass == "quality-ext" ? "04_quality_ext" : "02_quality_core")) / c.id / refn;
                    std::vector<uint16_t> ref;
                    bool haveRef = !args.refDir.empty() && TryLoadRawRgba16f(refFile, px.size(), ref);
                    FrameMetrics m = haveRef ? CompareRgba16f(ref, px) : FrameMetrics {};
                    frameMetrics[f] = m;
                    const char* phase = f < c.steady_frames ? "steady" : "drs";
                    csv << c.id << ',' << f << ',' << phase << ',' << rw << ',' << rh << ',' << reset << ','
                        << (haveRef ? "ref" : "self") << ',' << (haveRef ? refFile.filename().string() : "") << ','
                        << m.psnr01_db << ',' << m.psnr_reinhard_db << ',' << m.rgb_rmse << ',' << m.max_abs << ',' << m.mean_abs << ','
                        << m.max_ulp << ',' << m.exact_half_pct << ',' << m.non_finite << ',' << m.diff_halfs << '\n';
                    if (c.pass == "probes" && f == 15)
                    {
                        ownF015 = px;
                        if (!args.refDir.empty())
                        {
                            std::vector<uint16_t> ref5090;
                            if (TryLoadRawRgba16f(refFile, px.size(), ref5090)) f015Vs5090 = CompareRgba16f(ref5090, px);
                        }
                    }
                    if (f == 0 && c.pass == "probes") { /* kept for potential f000 comparisons */ }
                }
            }
            Require(api.v3.v2.v1.Destroy(ctx) == ONR_RR_OK, "Destroy failed");
        }
        else status = c.abi_not_exposed_yet ? std::string("abi_not_exposed_yet: ") + c.abi_note : "no-gpu";

        // Probes: self-equivalence columns (vs P00 and the pair ref) once P00 has run.
        if (c.pass == "probes")
        {
            probeF015Px[c.id] = ownF015;
            if (c.id == "P00_baseline_s1_g0") p00F015Sha = "saved";
        }

        // Summary row
        summary << (written++ ? ",\n" : "") << "    {\n"
                << "      \"id\": \"" << c.id << "\",\n"
                << "      \"status\": \"" << status << "\",\n";
        if (!frameMetrics.empty())
        {
            double worst = 999.0, worstRmse = 0.0;
            for (auto& [f, m] : frameMetrics) { worst = std::min(worst, m.psnr01_db); worstRmse = std::max(worstRmse, m.rgb_rmse); }
            auto at = [&](uint32_t f) { auto it = frameMetrics.find(f); return it == frameMetrics.end() ? -1.0 : it->second.psnr01_db; };
            summary << "      \"min_psnr01_db\": " << worst << ",\n"
                    << "      \"f000_psnr01_db\": " << at(0) << ",\n"
                    << "      \"f063_psnr01_db\": " << at(63) << ",\n"
                    << "      \"drs_f067_psnr01_db\": " << at(67) << ",\n"
                    << "      \"drs_f075_psnr01_db\": " << at(75) << ",\n"
                    << "      \"f015_vs_5090_psnr01_db\": " << f015Vs5090.psnr01_db << ",\n"
                    << "      \"f015_vs_5090_max_ulp\": " << f015Vs5090.max_ulp << ",\n"
                    << "      \"f015_vs_5090_rgb_rmse\": " << f015Vs5090.rgb_rmse << ",\n"
                    << "      \"f015_vs_5090_diff_halfs\": " << f015Vs5090.diff_halfs << ",\n";
        }
        summary << "      \"hash_gates\": " << c.expected_input_sha256.size() << "\n    }";
    }

    // Probe self columns (second pass over saved pixels). Pair mapping aligned with
    // ngx_rr_bench's pair_ref convention (plus P08->P07): a probe compares against its
    // designated neighbour so a shared-cause delta reads as 999 dB, an isolated cause as low.
    if (probeF015Px.count("P00_baseline_s1_g0"))
    {
        auto pairOf = [](const std::string& id) -> std::string {
            if (id == "P03_guide_transp_tlmv") return "P02_guide_transp_rgba";
            if (id == "P08_guide_dof_presetE") return "P07_guide_dof_presetF";
            if (id == "P09_no_hit_with_spec_mv") return "P05_guide_spec_mvec";
            if (id == "P13b_exposure_tex_1x1") return "P13a_exposure_no_auto";
            if (id == "P16b_mv_jittered_prev_minus_curr" || id == "P16c_mv_jittered_curr_minus_prev")
                return "P16a_mv_jittered_raw";
            if (id == "PX_combo_jitter_negscale") return "P16b_mv_jittered_prev_minus_curr";
            return {};
        };
        const auto& p00 = probeF015Px["P00_baseline_s1_g0"];
        std::ofstream pcsv(args.outDir / "probes_self.csv", std::ios::binary);
        pcsv << "case_id,f015_vs_p00_psnr01_db,f015_vs_p00_rgb_rmse,f015_vs_p00_max_ulp,f015_vs_p00_diff_halfs"
             << ",pair_ref_id,f015_vs_pair_psnr01_db,f015_vs_pair_rgb_rmse,f015_vs_pair_diff_halfs\n";
        for (const auto& [id, px] : probeF015Px)
        {
            if (id == "P00_baseline_s1_g0" || px.empty()) continue;   // skipped cases keep no row
            FrameMetrics m = CompareRgba16f(p00, px);
            pcsv << id << ',' << m.psnr01_db << ',' << m.rgb_rmse << ',' << m.max_ulp << ',' << m.diff_halfs;
            const std::string pair = pairOf(id);
            if (!pair.empty() && probeF015Px.count(pair) && !probeF015Px[pair].empty())
            {
                FrameMetrics mp = CompareRgba16f(probeF015Px[pair], px);
                pcsv << ',' << pair << ',' << mp.psnr01_db << ',' << mp.rgb_rmse << ',' << mp.diff_halfs;
            }
            else pcsv << ",,,,";   // fixed column count
            pcsv << '\n';
        }
    }

    summary << "\n  ]\n}\n";
    std::ofstream(args.outDir / "summary.json", std::ios::binary) << summary.str();
    std::printf("DONE %s / %s -> %s\n", args.suite.c_str(), cfg.name.c_str(), args.outDir.string().c_str());
    return 0;
}

int main(int argc, char** argv)
{
    Args args;
    args.outDir = "contrast-out";
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string { Require(i + 1 < argc, std::string(what) + " needs a value"); return argv[++i]; };
        if (a == "--asi") args.asi = next("--asi");
        else if (a == "--data-dir") args.dataDir = next("--data-dir");
        else if (a == "--dlssd") args.dlssd = next("--dlssd");
        else if (a == "--ref-dir") args.refDir = next("--ref-dir");
        else if (a == "--out") args.outDir = next("--out");
        else if (a == "--suite") args.suite = next("--suite");
        else if (a == "--config") args.config = next("--config");
        else if (a == "--cases") args.casesJson = next("--cases");
        else if (a == "--pass")
        {
            std::string v = next("--pass");
            std::string cur;
            std::stringstream ss(v);
            while (std::getline(ss, cur, ',')) args.onlyPasses.push_back(cur);
        }
        else if (a == "--case")
        {
            std::string v = next("--case");
            std::string cur;
            std::stringstream ss(v);
            while (std::getline(ss, cur, ',')) args.onlyCases.push_back(cur);
        }
        else if (a == "--no-gpu") args.noGpu = true;
        else Fail("unknown argument " + a);
    }
    Require(!args.asi.empty() || args.noGpu, "--asi required (or --no-gpu for hash-only)");
    Require(!args.dlssd.empty(), "--dlssd required");
    if (args.casesJson.empty())
    {
        const fs::path def = fs::path("C:/Users/Kenny/Downloads/OpenRR/build/RR_Contrast/cases") /
            (args.suite == "v2_motion" ? "cases_v2_motion.json" : "cases.json");
        args.casesJson = def;
    }
    try
    {
        return RunMain(args);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
