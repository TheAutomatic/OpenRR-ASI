#pragma once

#include "rr_path_u8.hpp"

#include <windows.h>

#include <cctype>
#include <cwchar>
#include <iterator>
#include <string>

namespace openrr::pe {

inline bool is_internal_vk_app_name(const char* name) {
    if (!name) return false;
    auto eq_ci = [](const char* s, const char* prefix, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (!s[i] || std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
        }
        return true;
    };
    return eq_ci(name, "dlssnr-amd", 10) || eq_ci(name, "openrr", 6);
}

template <class Pred, class Unwrap>
inline void* resolve_hookable_d3d12_target(void* slot_fn, Pred&& is_hookable, Unwrap&& unwrap_fn,
                                           std::string* out_mod = nullptr) {
    if (is_hookable(slot_fn, out_mod)) return slot_fn;
    void* unwrapped = unwrap_fn(slot_fn);
    std::string unwrapped_mod;
    if (unwrapped && unwrapped != slot_fn && is_hookable(unwrapped, out_mod ? &unwrapped_mod : nullptr)) {
        if (out_mod) *out_mod = std::move(unwrapped_mod);
        return unwrapped;
    }
    return slot_fn;
}

inline bool is_conflicting_d3d12_hook_module(const wchar_t* base) {
    if (!base) return false;
    return _wcsnicmp(base, L"dlssnr-amd", 10) == 0 || _wcsnicmp(base, L"openrr-amd", 10) == 0;
}

// True when `fn` resides inside a named loaded PE module that is neither `disallow_self` nor another
// OpenRR/DLSSNR-AMD plugin module. Refuses private memory (e.g. JIT/split vtable thunks) and NULL.
inline bool is_hookable_d3d12_target(const void* fn, HMODULE disallow_self, std::string* out_mod = nullptr) {
    if (!fn) {
        if (out_mod) *out_mod = "<null>";
        return false;
    }
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(fn), &mod) ||
        !mod) {
        if (out_mod) *out_mod = "private memory";
        return false;
    }
    wchar_t path[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        if (out_mod) *out_mod = "<unknown>";
        return false;
    }
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* fslash = std::wcsrchr(path, L'/');
    const wchar_t* sep = (slash && fslash) ? (slash > fslash ? slash : fslash) : (slash ? slash : fslash);
    const wchar_t* base = sep ? sep + 1 : path;
    if (out_mod) *out_mod = openrr::u8::narrow(base);
    if (disallow_self && mod == disallow_self) return false;
    if (is_conflicting_d3d12_hook_module(base)) return false;
    return true;
}

// Validates the command-list vtable slots that split patching inspects/replaces (Release, Close, Reset,
// SetDescriptorHeaps, SetComputeRootSignature). Returns false on the first non-hookable slot.
inline bool validate_split_list_vtable(void* const* vt, HMODULE disallow_self,
                                       unsigned* out_bad_slot = nullptr, std::string* out_bad_mod = nullptr) {
    if (!vt) {
        if (out_bad_slot) *out_bad_slot = 0;
        if (out_bad_mod) *out_bad_mod = "<null>";
        return false;
    }
    constexpr unsigned kCheckedSlots[] = {2u, 9u, 10u, 28u, 29u};
    for (const unsigned slot : kCheckedSlots) {
        std::string mod;
        if (!is_hookable_d3d12_target(vt[slot], disallow_self, &mod)) {
            if (out_bad_slot) *out_bad_slot = slot;
            if (out_bad_mod) *out_bad_mod = std::move(mod);
            return false;
        }
    }
    return true;
}

}  // namespace openrr::pe
