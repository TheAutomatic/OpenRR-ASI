// SPDX-License-Identifier: MIT
#include "rr_dxgi_queue.hpp"
#include <cstdio>
#include <thread>
using namespace openrr::pe::menu;
static int failures = 0;
static void expect(bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
static bool same_queue(IDXGISwapChain* sc, ID3D12CommandQueue* expected) {
    auto* q = dxgi_queue::get(sc);
    const bool same = q == expected;
    if (q) q->Release();
    return same;
}
int main(int argc, char** argv) {
    const bool after = argc > 1 && std::string(argv[1]) == "after";
    if (!after) expect(dxgi_queue::install(nullptr), "four system factory methods installed");
    IDXGIFactory2* factory = nullptr;
    ID3D12Device* device = nullptr;
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    Create create = &CreateDXGIFactory1;
    if (argc > 2) {
        auto module = LoadLibraryA(argv[2]);
        if (!module) return 4;
        create = reinterpret_cast<Create>(GetProcAddress(module, "CreateDXGIFactory1"));
        if (!create) return 4;
    }
    if (FAILED(create(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 2;
    if (after) expect(dxgi_queue::install(nullptr), "factory hooks after proxy factory creation");
    ID3D12CommandQueue *q = nullptr, *other = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other)))) return 2;
    HWND window = CreateWindowExW(0, L"STATIC", L"OpenRR queue test", WS_OVERLAPPEDWINDOW,
                                 0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 128; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferCount = 2; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    for (int generation = 0; generation < 3; ++generation) {
        IDXGISwapChain1* sc = nullptr;
        auto* expected = generation == 1 ? other : q;
        if (FAILED(factory->CreateSwapChainForHwnd(expected, window, &desc, nullptr, nullptr, &sc))) return 3;
        expect(same_queue(sc, expected), "same-thread exact queue identity / recreated chain");
        ID3D12CommandQueue* queried = nullptr;
        const HRESULT query = sc->GetDevice(IID_PPV_ARGS(&queried));
        std::printf("GetDevice(queue) = 0x%08lx\n", static_cast<unsigned long>(query));
        if (queried) queried->Release();
        other->ExecuteCommandLists(0, nullptr);
        std::thread presenter([&] {
            expect(same_queue(sc, expected), "cross-thread identity after unrelated DIRECT submission");
            expect(SUCCEEDED(sc->Present(0, 0)), "cross-thread Present");
        });
        presenter.join();
        expect(SUCCEEDED(sc->ResizeBuffers(2, 128, 128, desc.Format, 0)), "ResizeBuffers");
        expect(same_queue(sc, expected), "queue retained across ResizeBuffers");
        sc->Release();
    }
    IDXGISwapChain1* composition = nullptr;
    expect(SUCCEEDED(factory->CreateSwapChainForComposition(q, &desc, nullptr, &composition)), "composition creation");
    if (composition) { expect(same_queue(composition, q), "composition queue identity"); composition->Release(); }
    DXGI_SWAP_CHAIN_DESC legacy{};
    legacy.BufferDesc.Width = legacy.BufferDesc.Height = 128;
    legacy.BufferDesc.Format = desc.Format; legacy.SampleDesc.Count = 1;
    legacy.BufferUsage = desc.BufferUsage; legacy.BufferCount = 2;
    legacy.OutputWindow = window; legacy.Windowed = TRUE;
    legacy.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* old = nullptr;
    expect(SUCCEEDED(factory->CreateSwapChain(other, &legacy, &old)), "legacy creation");
    if (old) { expect(same_queue(old, other), "legacy queue identity"); old->Release(); }
    DestroyWindow(window);
    other->Release(); q->Release(); device->Release(); factory->Release();
    return failures ? 1 : 0;
}
