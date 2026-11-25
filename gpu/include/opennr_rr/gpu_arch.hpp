#pragma once
// GPU architecture identification from LLVM EF_AMDGPU_MACH (e_flags) and explicit tables.
// Never uses overlapping hex ranges or conflates architecture identity with ACO record availability.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace onr::arch {

enum class GpuArch : uint8_t {
    Unknown = 0,
    Rdna1,
    Rdna2,
    Rdna3,
    Rdna4,
};

inline const char* arch_name(GpuArch a) {
    switch (a) {
    case GpuArch::Rdna1: return "RDNA1";
    case GpuArch::Rdna2: return "RDNA2";
    case GpuArch::Rdna3: return "RDNA3";
    case GpuArch::Rdna4: return "RDNA4";
    case GpuArch::Unknown: break;
    }
    return "unknown";
}

inline bool has_runtime_support(GpuArch a) {
    return a == GpuArch::Rdna3 || a == GpuArch::Rdna4;
}

struct MachineEntry {
    uint8_t mach;
    const char* gfx;
    GpuArch arch;
};

// Explicit EF_AMDGPU_MACH values from LLVM BinaryFormat/ELF.h (no overlapping hex ranges).
inline constexpr MachineEntry kMachines[] = {
    {0x33, "gfx1010", GpuArch::Rdna1}, {0x34, "gfx1011", GpuArch::Rdna1},
    {0x35, "gfx1012", GpuArch::Rdna1}, {0x42, "gfx1013", GpuArch::Rdna1},
    {0x52, "gfx10-1-generic", GpuArch::Rdna1},
    {0x36, "gfx1030", GpuArch::Rdna2}, {0x37, "gfx1031", GpuArch::Rdna2},
    {0x38, "gfx1032", GpuArch::Rdna2}, {0x39, "gfx1033", GpuArch::Rdna2},
    {0x3d, "gfx1035", GpuArch::Rdna2}, {0x3e, "gfx1034", GpuArch::Rdna2},
    {0x45, "gfx1036", GpuArch::Rdna2}, {0x53, "gfx10-3-generic", GpuArch::Rdna2},
    {0x41, "gfx1100", GpuArch::Rdna3}, {0x46, "gfx1101", GpuArch::Rdna3},
    {0x47, "gfx1102", GpuArch::Rdna3}, {0x44, "gfx1103", GpuArch::Rdna3},
    {0x43, "gfx1150", GpuArch::Rdna3}, {0x4a, "gfx1151", GpuArch::Rdna3},
    {0x55, "gfx1152", GpuArch::Rdna3}, {0x58, "gfx1153", GpuArch::Rdna3},
    {0x54, "gfx11-generic", GpuArch::Rdna3},
    {0x48, "gfx1200", GpuArch::Rdna4}, {0x4e, "gfx1201", GpuArch::Rdna4},
    {0x59, "gfx12-generic", GpuArch::Rdna4},
};

inline const MachineEntry* lookup_mach(uint32_t mach) {
    const uint8_t m = uint8_t(mach & 0xffu);
    for (const MachineEntry& e : kMachines) {
        if (e.mach == m) return &e;
    }
    return nullptr;
}

inline GpuArch arch_from_mach(uint32_t mach) {
    const MachineEntry* e = lookup_mach(mach);
    return e ? e->arch : GpuArch::Unknown;
}

inline const char* gfx_name_from_mach(uint32_t mach) {
    const MachineEntry* e = lookup_mach(mach);
    return e ? e->gfx : nullptr;
}

// Offline ACO target directory/manifest name for discrete validated ELF machine IDs.
// Generic targets (e.g. 0x59 gfx12-generic) and RDNA3 targets intentionally return nullptr.
inline const char* aco_target_for_mach(uint32_t mach) {
    switch (mach & 0xffu) {
    case 0x4e: return "gfx1201";
    case 0x48: return "gfx1200";
    default: return nullptr;
    }
}

struct PciEntry {
    uint16_t device_id;
    GpuArch arch;
};

// Explicit AMD graphics PCI device ID fallback (used only when driver compiler mach is unavailable).
inline constexpr PciEntry kPciDevices[] = {
    // RDNA4: Navi 48 (RX 9070 / 9070 XT / 9070 GRE, Radeon AI PRO R9700), Navi 44 (RX 9060 / 9060 XT)
    {0x7550, GpuArch::Rdna4}, {0x7551, GpuArch::Rdna4}, {0x7590, GpuArch::Rdna4},
    // RDNA3: Navi 31 / Navi 32 / Navi 33 + RDNA3/3.5 APUs (Phoenix 780M, Hawk Point, Strix Point 890M, Strix Halo 8060S, Krackan)
    {0x7448, GpuArch::Rdna3}, {0x7449, GpuArch::Rdna3}, {0x744a, GpuArch::Rdna3}, {0x744b, GpuArch::Rdna3},
    {0x744c, GpuArch::Rdna3}, {0x745e, GpuArch::Rdna3},
    {0x7460, GpuArch::Rdna3}, {0x7461, GpuArch::Rdna3}, {0x7470, GpuArch::Rdna3}, {0x747e, GpuArch::Rdna3},
    {0x73f0, GpuArch::Rdna3}, {0x7480, GpuArch::Rdna3}, {0x7481, GpuArch::Rdna3}, {0x7483, GpuArch::Rdna3},
    {0x7487, GpuArch::Rdna3}, {0x7489, GpuArch::Rdna3}, {0x748b, GpuArch::Rdna3}, {0x7499, GpuArch::Rdna3},
    {0x749f, GpuArch::Rdna3},
    {0x15bf, GpuArch::Rdna3}, {0x15c8, GpuArch::Rdna3}, {0x1900, GpuArch::Rdna3}, {0x1901, GpuArch::Rdna3},
    {0x1902, GpuArch::Rdna3}, {0x150e, GpuArch::Rdna3}, {0x1586, GpuArch::Rdna3}, {0x1114, GpuArch::Rdna3},
    // RDNA2: Navi 21 / 22 / 23 / 24 + RDNA2 APUs
    {0x73a1, GpuArch::Rdna2}, {0x73a2, GpuArch::Rdna2}, {0x73a3, GpuArch::Rdna2}, {0x73a5, GpuArch::Rdna2},
    {0x73ab, GpuArch::Rdna2}, {0x73ae, GpuArch::Rdna2}, {0x73af, GpuArch::Rdna2}, {0x73bf, GpuArch::Rdna2},
    {0x73c3, GpuArch::Rdna2}, {0x73ce, GpuArch::Rdna2}, {0x73df, GpuArch::Rdna2},
    {0x73e0, GpuArch::Rdna2}, {0x73e1, GpuArch::Rdna2}, {0x73e3, GpuArch::Rdna2}, {0x73ef, GpuArch::Rdna2},
    {0x73ff, GpuArch::Rdna2},
    {0x7421, GpuArch::Rdna2}, {0x7422, GpuArch::Rdna2}, {0x7423, GpuArch::Rdna2}, {0x7424, GpuArch::Rdna2},
    {0x743f, GpuArch::Rdna2},
    {0x163f, GpuArch::Rdna2}, {0x1435, GpuArch::Rdna2}, {0x1681, GpuArch::Rdna2}, {0x164e, GpuArch::Rdna2},
    {0x13c0, GpuArch::Rdna2}, {0x1506, GpuArch::Rdna2},
    // RDNA1: Navi 10 / 12 / 14
    {0x7310, GpuArch::Rdna1}, {0x7312, GpuArch::Rdna1}, {0x7319, GpuArch::Rdna1}, {0x731b, GpuArch::Rdna1},
    {0x731f, GpuArch::Rdna1}, {0x7340, GpuArch::Rdna1}, {0x7341, GpuArch::Rdna1}, {0x7347, GpuArch::Rdna1},
    {0x734f, GpuArch::Rdna1}, {0x7360, GpuArch::Rdna1}, {0x7362, GpuArch::Rdna1},
};

inline GpuArch arch_from_pci(uint32_t vendor, uint32_t device) {
    if (vendor != 0x1002u) return GpuArch::Unknown;
    for (const PciEntry& d : kPciDevices) {
        if (d.device_id == device) return d.arch;
    }
    return GpuArch::Unknown;
}

inline uint32_t parse_elf_mach(const void* data, size_t bytes) {
    if (!data || bytes < 64) return 0;
    const auto* d = static_cast<const uint8_t*>(data);
    if (std::memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2 || d[5] != 1) return 0;
    const uint16_t e_machine = uint16_t(d[18] | (uint16_t(d[19]) << 8));
    if (e_machine != 224) return 0;   // EM_AMDGPU
    const uint32_t e_flags = uint32_t(d[48]) | (uint32_t(d[49]) << 8) |
                             (uint32_t(d[50]) << 16) | (uint32_t(d[51]) << 24);
    return e_flags & 0xffu;
}

}  // namespace onr::arch
