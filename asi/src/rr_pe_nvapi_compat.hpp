// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include "MinHook.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#if defined(__GNUC__) && defined(_WIN64)
#define OPENRR_NVAPI_MSABI __attribute__((ms_abi))
#else
#define OPENRR_NVAPI_MSABI
#endif

#if defined(__GNUC__)
#define OPENRR_NVAPI_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define OPENRR_NVAPI_NOINLINE __declspec(noinline)
#else
#define OPENRR_NVAPI_NOINLINE
#endif

namespace openrr::pe::nvapi {

inline constexpr uint32_t kInterfaceIdGpuGetGpuInfo = 0xafd1b02cu;

inline constexpr int kStatusOk = 0;
inline constexpr int kStatusNoImplementation = -3;
inline constexpr int kStatusInvalidArgument = -5;
inline constexpr int kStatusIncompatibleStructVersion = -9;
inline constexpr int kStatusExpectedPhysicalGpuHandle = -101;

struct GpuInfoV1 {
    uint32_t version;
    uint32_t flags;
};
static_assert(sizeof(GpuInfoV1) == 8);
inline constexpr uint32_t kGpuInfoVer1 = (1u << 16) | static_cast<uint32_t>(sizeof(GpuInfoV1));

struct GpuInfoV2 {
    uint32_t version;
    uint32_t flags;
    uint64_t reserved1;
    uint32_t ray_tracing_cores;
    uint32_t tensor_cores;
    uint32_t reserved2[14];
};
static_assert(sizeof(GpuInfoV2) == 0x50);
static_assert(offsetof(GpuInfoV2, ray_tracing_cores) == 0x10);
static_assert(offsetof(GpuInfoV2, tensor_cores) == 0x14);
inline constexpr uint32_t kGpuInfoVer2 = (2u << 16) | static_cast<uint32_t>(sizeof(GpuInfoV2));

// Matches dxvk-nvapi's RTX 4080 defaults for NV_GPU_INFO_VER2.
inline constexpr uint32_t kDefaultRayTracingCores = 76u;
inline constexpr uint32_t kDefaultTensorCores = 304u;

inline int fill_gpu_info(const void* physical_gpu, void* gpu_info,
                         uint32_t rt_cores = kDefaultRayTracingCores,
                         uint32_t tensor_cores = kDefaultTensorCores) {
    // OptiScaler's fakenvapi::NvAPI_EnumPhysicalGPUs sets handles[0] = nullptr, so callers pass null handles.
    (void)physical_gpu;
    if (!gpu_info) return kStatusInvalidArgument;

    uint32_t ver = 0;
    std::memcpy(&ver, gpu_info, sizeof(ver));
    if (ver == kGpuInfoVer1) {
        GpuInfoV1 out{};
        out.version = kGpuInfoVer1;
        std::memcpy(gpu_info, &out, sizeof(out));
        return kStatusOk;
    }
    if (ver == kGpuInfoVer2) {
        GpuInfoV2 out{};
        out.version = kGpuInfoVer2;
        out.ray_tracing_cores = rt_cores;
        out.tensor_cores = tensor_cores;
        std::memcpy(gpu_info, &out, sizeof(out));
        return kStatusOk;
    }
    return kStatusIncompatibleStructVersion;
}

inline int OPENRR_NVAPI_MSABI gpu_get_gpu_info_stub(const void* physical_gpu, void* gpu_info) {
    return fill_gpu_info(physical_gpu, gpu_info);
}

using PFN_NvApiQueryInterface = void*(OPENRR_NVAPI_MSABI*)(uint32_t);
using PFN_NvApiGpuGetGpuInfo = int(OPENRR_NVAPI_MSABI*)(const void*, void*);
using CanProvideFn = bool (*)();
using LogFn = void (*)(const char*);

// Multi-slot MinHook manager for nvapi_QueryInterface exports (OptiScaler built-in or standalone nvapi64.dll).
// Intercepts kInterfaceIdGpuGetGpuInfo (0xafd1b02c) via a per-slot wrapper when enabled so placeholder or
// unimplemented underlying entries fall back cleanly while real NVIDIA implementations pass through untouched.
template <size_t MaxSlots = 4, typename Tag = void>
class QueryInterfaceHooks {
public:
    QueryInterfaceHooks() { s_self_.store(this, std::memory_order_release); }
    ~QueryInterfaceHooks() {
        QueryInterfaceHooks* expected = this;
        s_self_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
    QueryInterfaceHooks(const QueryInterfaceHooks&) = delete;
    QueryInterfaceHooks& operator=(const QueryInterfaceHooks&) = delete;

    void set_callbacks(CanProvideFn can_provide, LogFn log_fn = nullptr) {
        can_provide_.store(can_provide, std::memory_order_release);
        log_fn_.store(log_fn, std::memory_order_release);
    }

    // Must not invoke can_provide() here: may be called from DllMain / LdrDllNotification under loader lock.
    bool hook_function(void* fn) {
        if (!fn) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        s_self_.store(this, std::memory_order_release);

        size_t free_slot = MaxSlots;
        for (size_t i = 0; i < MaxSlots; ++i) {
            void* existing = slots_[i].target.load(std::memory_order_relaxed);
            if (existing == fn) return true;
            if (!existing && free_slot == MaxSlots) free_slot = i;
        }
        if (free_slot >= MaxSlots) return false;

        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;

        static constexpr auto detours = make_detours(std::make_index_sequence<MaxSlots>{});
        void* orig = nullptr;
        if (MH_CreateHook(fn, reinterpret_cast<void*>(detours[free_slot]), &orig) != MH_OK) return false;

        slots_[free_slot].underlying_get_gpu_info.store(nullptr, std::memory_order_release);
        slots_[free_slot].orig.store(reinterpret_cast<PFN_NvApiQueryInterface>(orig), std::memory_order_release);
        if (MH_EnableHook(fn) != MH_OK) {
            MH_RemoveHook(fn);
            slots_[free_slot].orig.store(nullptr, std::memory_order_release);
            return false;
        }
        slots_[free_slot].target.store(fn, std::memory_order_release);
        return true;
    }

    bool hook_module(HMODULE mod) {
        if (!mod) return false;
        void* fn = reinterpret_cast<void*>(GetProcAddress(mod, "nvapi_QueryInterface"));
        if (!fn) return false;
        return hook_function(fn);
    }

    void unhook_module(const void* base, size_t size) {
        if (!base || size == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto begin = reinterpret_cast<uintptr_t>(base);
        for (size_t i = 0; i < MaxSlots; ++i) {
            void* fn = slots_[i].target.load(std::memory_order_relaxed);
            if (!fn) continue;
            const auto at = reinterpret_cast<uintptr_t>(fn);
            if (at >= begin && (at - begin) < size) {
                MH_RemoveHook(fn);
                slots_[i].target.store(nullptr, std::memory_order_release);
                slots_[i].orig.store(nullptr, std::memory_order_release);
                slots_[i].underlying_get_gpu_info.store(nullptr, std::memory_order_release);
            }
        }
    }

private:
    struct SlotState {
        std::atomic<void*> target{nullptr};
        std::atomic<PFN_NvApiQueryInterface> orig{nullptr};
        std::atomic<PFN_NvApiGpuGetGpuInfo> underlying_get_gpu_info{nullptr};
    };

    OPENRR_NVAPI_NOINLINE void log_fallback_once(bool has_underlying) {
        if (!logged_once_.exchange(true, std::memory_order_relaxed)) {
            if (LogFn lf = log_fn_.load(std::memory_order_acquire)) {
                lf(has_underlying
                       ? "NvAPI_GPU_GetGPUInfo (0xafd1b02c): providing fallback (underlying placeholder)"
                       : "NvAPI_GPU_GetGPUInfo (0xafd1b02c): providing fallback (underlying null)");
            }
        }
    }

    template <size_t Slot>
    OPENRR_NVAPI_NOINLINE static int OPENRR_NVAPI_MSABI gpu_info_wrapper_slot(const void* physical_gpu,
                                                                              void* gpu_info) {
        uint32_t saved_ver = 0;
        if (gpu_info) std::memcpy(&saved_ver, gpu_info, sizeof(saved_ver));

        QueryInterfaceHooks* self = s_self_.load(std::memory_order_acquire);
        PFN_NvApiGpuGetGpuInfo underlying =
            self ? self->slots_[Slot].underlying_get_gpu_info.load(std::memory_order_acquire) : nullptr;
        if (underlying != nullptr) {
            const int st = underlying(physical_gpu, gpu_info);
            if (st == kStatusOk) {
                if (gpu_info && saved_ver == kGpuInfoVer2) {
                    GpuInfoV2 out{};
                    std::memcpy(&out, gpu_info, sizeof(out));
                    if (out.ray_tracing_cores > 0) return kStatusOk;
                } else {
                    return kStatusOk;
                }
            }
            if (gpu_info) std::memcpy(gpu_info, &saved_ver, sizeof(saved_ver));
        }

        if (self) self->log_fallback_once(underlying != nullptr);
        return fill_gpu_info(physical_gpu, gpu_info);
    }

    template <size_t... Is>
    static constexpr std::array<PFN_NvApiGpuGetGpuInfo, sizeof...(Is)> make_wrappers(std::index_sequence<Is...>) {
        return {&gpu_info_wrapper_slot<Is>...};
    }

    OPENRR_NVAPI_NOINLINE void* wrap_get_gpu_info(size_t slot, void* underlying) {
        CanProvideFn cp = can_provide_.load(std::memory_order_acquire);
        if (!cp || !cp()) return underlying;

        static constexpr auto wrappers = make_wrappers(std::make_index_sequence<MaxSlots>{});
        void* wrapper = reinterpret_cast<void*>(wrappers[slot]);
        if (underlying == wrapper) return wrapper;

        slots_[slot].underlying_get_gpu_info.store(
            reinterpret_cast<PFN_NvApiGpuGetGpuInfo>(underlying), std::memory_order_release);
        return wrapper;
    }

    // Distinct per-slot instantiations with full prologues so OptiScaler's Detours can chain cleanly if attached.
    template <size_t Slot>
    OPENRR_NVAPI_NOINLINE static void* OPENRR_NVAPI_MSABI detour_slot(uint32_t id) {
        QueryInterfaceHooks* self = s_self_.load(std::memory_order_acquire);
        PFN_NvApiQueryInterface orig = self ? self->slots_[Slot].orig.load(std::memory_order_acquire) : nullptr;
        void* fn = orig ? orig(id) : nullptr;
        if (id != kInterfaceIdGpuGetGpuInfo || !self) return fn;
        return self->wrap_get_gpu_info(Slot, fn);
    }

    template <size_t... Is>
    static constexpr std::array<PFN_NvApiQueryInterface, sizeof...(Is)> make_detours(std::index_sequence<Is...>) {
        return {&detour_slot<Is>...};
    }

    SlotState slots_[MaxSlots]{};
    std::atomic<CanProvideFn> can_provide_{nullptr};
    std::atomic<LogFn> log_fn_{nullptr};
    std::atomic<bool> logged_once_{false};
    std::mutex mutex_;
    static inline std::atomic<QueryInterfaceHooks*> s_self_{nullptr};
};

} // namespace openrr::pe::nvapi
