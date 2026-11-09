// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace openrr::pe {
// Deliberately accepts only the standard x64 MinHook relay + terminal jump-back
// layout. Unknown layouts stay on the ordinary export-hook path.
inline bool readable_code(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(m.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const auto a = reinterpret_cast<uintptr_t>(p);
    return a >= reinterpret_cast<uintptr_t>(m.BaseAddress) &&
           n <= m.RegionSize && a - reinterpret_cast<uintptr_t>(m.BaseAddress) <= m.RegionSize - n;
}
inline uintptr_t absolute_jump(const uint8_t* p) {
    if (!readable_code(p, 14) || p[0] != 0xff || p[1] != 0x25 ||
        p[2] || p[3] || p[4] || p[5]) return 0;
    uintptr_t target = 0;
    std::memcpy(&target, p + 6, sizeof(target));
    return target;
}
inline void* minhook_inner_target(void* entry, uintptr_t owner_begin, uintptr_t owner_end) {
    auto* fn = static_cast<uint8_t*>(entry);
    if (!readable_code(fn, 5) || fn[0] != 0xe9) return nullptr;
    int32_t displacement = 0;
    std::memcpy(&displacement, fn + 1, sizeof(displacement));
    const auto relay_addr = reinterpret_cast<uintptr_t>(fn) + 5 + displacement;
    auto* relay = reinterpret_cast<uint8_t*>(relay_addr);
    const auto detour = absolute_jump(relay);
    if (detour < owner_begin || detour >= owner_end) return nullptr;
    const auto slot_addr = relay_addr & ~uintptr_t(63);
    const auto offset = relay_addr - slot_addr;
    // At least five bytes of relocated code, a 14-byte jump-back, and relay.
    if (offset < 19 || offset + 14 > 64) return nullptr;
    auto* slot = reinterpret_cast<uint8_t*>(slot_addr);
    if (!readable_code(slot, 64)) return nullptr;
    const auto back = absolute_jump(relay - 14);
    const auto original = reinterpret_cast<uintptr_t>(fn);
    if (back < original + 5 || back > original + 19) return nullptr;
    // A slot already patched by another consumer is not ours to rearrange.
    if (slot[0] == 0xe9 || slot[0] == 0xeb || absolute_jump(slot)) return nullptr;
    return slot;
}
} // namespace openrr::pe
