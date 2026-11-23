// SPDX-License-Identifier: MIT
#pragma once

#include "rr_path_u8.hpp"

#include <windows.h>

#include <cwchar>
#include <iterator>
#include <string>

namespace openrr::pe {

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

}  // namespace openrr::pe
