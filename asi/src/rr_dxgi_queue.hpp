// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <mutex>
#include <tuple>
#include <string>
#include "MinHook.h"

namespace openrr::pe::menu::dxgi_queue {
// Private data is shared by all interfaces of a DXGI object. DXGI retains/releases
// the queue, so neither swap-chain pointer reuse nor destruction needs a cache.
inline const GUID queue_key = {0xa7d07323, 0x811e, 0x42ac, {0xa0, 0x67, 0x4c, 0x35, 0x93, 0x58, 0x7b, 0x21}};
inline bool remember(IDXGISwapChain* sc, IUnknown* device) {
    ID3D12CommandQueue* queue = nullptr;
    if (!sc || !device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return false;
    const HRESULT hr = sc->SetPrivateDataInterface(queue_key, queue);
    queue->Release();
    return SUCCEEDED(hr);
}
inline ID3D12CommandQueue* get(IDXGISwapChain* sc) {
    IUnknown* stored = nullptr;
    UINT size = sizeof(stored);
    if (FAILED(sc->GetPrivateData(queue_key, &size, &stored)) || !stored) return nullptr;
    ID3D12CommandQueue* queue = nullptr;
    stored->QueryInterface(IID_PPV_ARGS(&queue));
    stored->Release();
    return queue; // Owns one reference.
}

using Seen = void (*)(IDXGISwapChain*);
inline Seen seen = nullptr; // Published before enabling hooks; immutable afterwards.
template<class Fn, int Slot> struct Creation;
template<class Factory, class... Args, int Slot>
struct Creation<HRESULT(STDMETHODCALLTYPE*)(Factory*, IUnknown*, Args...), Slot> {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(Factory*, IUnknown*, Args...);
    static inline Fn original = nullptr;
    static HRESULT STDMETHODCALLTYPE call(Factory* factory, IUnknown* device, Args... args) {
        const HRESULT hr = original(factory, device, args...);
        auto out = std::get<sizeof...(Args) - 1>(std::tuple<Args...>(args...));
        if (SUCCEEDED(hr) && out && *out) {
            // Instrumentation must not change the factory method's result.
            try { remember(*out, device); if (seen) seen(*out); } catch (...) {}
        }
        return hr;
    }
    static bool install(void** vt) {
        if (original) return true;
        void* trampoline = nullptr;
        const auto status = MH_CreateHook(vt[Slot], reinterpret_cast<void*>(&call), &trampoline);
        if (status != MH_OK) return false;
        original = reinterpret_cast<Fn>(trampoline);
        if (MH_EnableHook(vt[Slot]) == MH_OK) return true;
        MH_RemoveHook(vt[Slot]);
        original = nullptr;
        return false;
    }
};
using Legacy = Creation<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**), 10>;
using Hwnd = Creation<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**), 15>;
using Core = Creation<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**), 16>;
using Composition = Creation<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**), 24>;

// Call synchronously outside loader lock, before returning the first D3D12 device.
// No device/window probe and no hooks on factory exports (including proxy exports).
inline bool install(Seen callback) {
    static std::mutex mutex;
    static bool ready = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (ready) return true;
    wchar_t dir[MAX_PATH]{};
    if (!GetSystemDirectoryW(dir, MAX_PATH)) return false;
    const std::wstring path = std::wstring(dir) + L"/dxgi.dll";
    HMODULE module = GetModuleHandleW(path.c_str());
    if (!module) module = LoadLibraryW(path.c_str());
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory2* factory = nullptr;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory) return false;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { factory->Release(); return false; }
    if (!Legacy::original && !Hwnd::original && !Core::original && !Composition::original) seen = callback;
    void** vt = *reinterpret_cast<void***>(factory);
    // Attempt all four even if one fails; a later call retries missing hooks.
    const bool a = Legacy::install(vt), b = Hwnd::install(vt);
    const bool c = Core::install(vt), d = Composition::install(vt);
    factory->Release();
    return ready = a && b && c && d;
}
} // namespace openrr::pe::menu::dxgi_queue
