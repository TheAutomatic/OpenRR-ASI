// SPDX-License-Identifier: MIT
#include "opennr_rr/vk.hpp"
#include "rr_pipeline_binary.hpp"
#include "rr_radv_spirv.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <winreg.h>
#include <cwchar>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#endif

namespace onr {

std::filesystem::path u8_to_path(const std::string& s) {
#ifdef _WIN32
    std::wstring w;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        unsigned c = *p, n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
        else { w.push_back(wchar_t(0xFFFD)); ++p; continue; }
        if (p + n >= end) { w.push_back(wchar_t(0xFFFD)); break; }
        unsigned k = 1;
        for (; k <= n; ++k) {
            if ((p[k] & 0xC0) != 0x80) break;
            c = (c << 6) | (p[k] & 0x3F);
        }
        if (k <= n) { w.push_back(wchar_t(0xFFFD)); p += k; continue; }
        p += n + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            w.push_back(wchar_t(0xD800 + (c >> 10)));
            w.push_back(wchar_t(0xDC00 + (c & 0x3FF)));
        } else {
            w.push_back(wchar_t(c));
        }
    }
    return std::filesystem::path(w);
#else
    return std::filesystem::path(s);
#endif
}

std::string path_to_u8(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring w = p.wstring();
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) {
        unsigned c = unsigned(w[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size() && unsigned(w[i + 1]) >= 0xDC00 && unsigned(w[i + 1]) < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (unsigned(w[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) s.push_back(char(c));
        else if (c < 0x800) {
            s.push_back(char(0xC0 | (c >> 6)));
            s.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            s.push_back(char(0xE0 | (c >> 12)));
            s.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(char(0x80 | (c & 0x3F)));
        } else {
            s.push_back(char(0xF0 | (c >> 18)));
            s.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            s.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            s.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return s;
#else
    return p.string();
#endif
}

namespace {
struct VkCmdTrack {
    VkDevice dev = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint64_t cmd_epoch = 0;
    bool freed = false;
};
struct VkPoolTrack {
    VkDevice dev = VK_NULL_HANDLE;
    uint64_t pool_epoch = 0;
    bool destroyed = false;
};

std::mutex g_vk_track_m;
std::atomic<uint64_t> g_vk_next_epoch{1};
std::unordered_map<VkCommandBuffer, VkCmdTrack> g_cmd_track;
std::unordered_map<VkCommandPool, VkPoolTrack> g_pool_track;
std::unordered_set<VkDevice> g_destroyed_vk_devices;

void bind_vk_cmd_epoch(VkDevice dev, VkCommandBuffer cmd, uint64_t& out_cmd_epoch, VkCommandPool& out_pool, uint64_t& out_pool_epoch) {
    out_cmd_epoch = 0;
    out_pool = VK_NULL_HANDLE;
    out_pool_epoch = 0;
    if (!cmd) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    auto it = g_cmd_track.find(cmd);
    if (it == g_cmd_track.end() || it->second.freed || it->second.cmd_epoch == 0) {
        const uint64_t ce = g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed);
        g_cmd_track[cmd] = VkCmdTrack{dev, VK_NULL_HANDLE, ce, false};
        out_cmd_epoch = ce;
        return;
    }
    out_cmd_epoch = it->second.cmd_epoch;
    if (it->second.pool) {
        auto pit = g_pool_track.find(it->second.pool);
        if (pit != g_pool_track.end() && !pit->second.destroyed && pit->second.pool_epoch != 0) {
            out_pool = it->second.pool;
            out_pool_epoch = pit->second.pool_epoch;
        }
    }
}

bool vk_cmd_retired(VkCommandBuffer cmd, uint64_t rec_cmd_epoch, VkCommandPool pool, uint64_t rec_pool_epoch) {
    if (!cmd || rec_cmd_epoch == 0) return false;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    auto it = g_cmd_track.find(cmd);
    if (it != g_cmd_track.end() && it->second.cmd_epoch > rec_cmd_epoch)
        return true;
    if (pool && rec_pool_epoch != 0) {
        auto pit = g_pool_track.find(pool);
        if (pit != g_pool_track.end() && pit->second.pool_epoch > rec_pool_epoch)
            return true;
    }
    return false;
}
}  // namespace

void vk_note_command_buffers_allocated(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cmds) {
    if (!cmds || !count) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    if (dev) g_destroyed_vk_devices.erase(dev);
    if (pool) {
        auto pit = g_pool_track.find(pool);
        if (pit == g_pool_track.end() || pit->second.destroyed || pit->second.pool_epoch == 0)
            g_pool_track[pool] = VkPoolTrack{dev, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), false};
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!cmds[i]) continue;
        g_cmd_track[cmds[i]] = VkCmdTrack{dev, pool, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), false};
    }
}

void vk_note_command_buffer_reset(VkCommandBuffer cmd) {
    if (!cmd) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    auto it = g_cmd_track.find(cmd);
    if (it != g_cmd_track.end()) {
        it->second.cmd_epoch = g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed);
        it->second.freed = false;
    } else {
        g_cmd_track[cmd] = VkCmdTrack{VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), false};
    }
}

void vk_note_command_buffers_freed(VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cmds) {
    if (!cmds || !count) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    for (uint32_t i = 0; i < count; ++i) {
        if (!cmds[i]) continue;
        auto it = g_cmd_track.find(cmds[i]);
        if (it != g_cmd_track.end()) {
            it->second.cmd_epoch = g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed);
            it->second.freed = true;
        } else {
            g_cmd_track[cmds[i]] = VkCmdTrack{dev, pool, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), true};
        }
    }
}

void vk_note_command_pool_reset(VkDevice dev, VkCommandPool pool) {
    if (!pool) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    g_pool_track[pool] = VkPoolTrack{dev, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), false};
    for (auto& kv : g_cmd_track) {
        if (kv.second.pool == pool)
            kv.second.cmd_epoch = g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void vk_note_command_pool_destroyed(VkDevice dev, VkCommandPool pool) {
    if (!pool) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    g_pool_track[pool] = VkPoolTrack{dev, g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed), true};
    for (auto& kv : g_cmd_track) {
        if (kv.second.pool == pool) {
            kv.second.cmd_epoch = g_vk_next_epoch.fetch_add(1, std::memory_order_relaxed);
            kv.second.freed = true;
        }
    }
}

std::unordered_set<VkDevice> g_pipeline_binary_devs;

void vk_note_device_destroyed(VkDevice dev) {
    if (!dev) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    g_destroyed_vk_devices.insert(dev);
    g_pipeline_binary_devs.erase(dev);
    std::erase_if(g_cmd_track, [&](const auto& kv) { return kv.second.dev == dev; });
    std::erase_if(g_pool_track, [&](const auto& kv) { return kv.second.dev == dev; });
}

bool vk_device_is_destroyed(VkDevice dev) {
    if (!dev) return true;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    return g_destroyed_vk_devices.count(dev) != 0;
}

void vk_set_device_pipeline_binary(VkDevice dev, bool enabled) {
    if (!dev) return;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    g_destroyed_vk_devices.erase(dev);
    if (enabled) {
        g_pipeline_binary_devs.insert(dev);
    } else {
        g_pipeline_binary_devs.erase(dev);
    }
}

bool vk_device_has_pipeline_binary(VkDevice dev) {
    if (!dev) return false;
    std::lock_guard<std::mutex> lock(g_vk_track_m);
    return g_destroyed_vk_devices.count(dev) == 0 && g_pipeline_binary_devs.count(dev) != 0;
}

bool env_flag_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
    return it != hay.end();
}

#ifdef _WIN32
struct CachedAmdIdentity {
    TrustedVkIdentity id;
};
std::mutex g_real_props_m;
std::unordered_map<uint64_t, CachedAmdIdentity> g_real_props_cache;

// Verified 66-word minimal Vulkan 1.3 compute shader (local_size_x = 1) used only inside direct amdvlk64.dll ICD probe
// to extract exact LLVM EF_AMDGPU_MACH via VK_AMD_shader_info without trusting loader/layer hooks.
static constexpr uint32_t kProbeSpv[66] = {
    0x07230203u, 0x00010600u, 0x0008000bu, 0x0000000au, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060010u, 0x00000004u, 0x00000011u,
    0x00000001u, 0x00000001u, 0x00000001u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u,
    0x6e69616du, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u,
    0x00000006u, 0x00000020u, 0x00000000u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000003u, 0x0004002bu,
    0x00000006u, 0x00000008u, 0x00000001u, 0x0006002cu, 0x00000007u, 0x00000009u, 0x00000008u, 0x00000008u,
    0x00000008u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
    0x000100fdu, 0x00010038u,
};

uint32_t probe_icd_elf_mach(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd) {
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst, "vkEnumerateDeviceExtensionProperties"));
    auto get_qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto create_dev = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
    if (!enum_ext || !get_qfp || !create_dev || !gdpa) return 0;

    uint32_t ne = 0;
    if (enum_ext(pd, nullptr, &ne, nullptr) != VK_SUCCESS || !ne) return 0;
    std::vector<VkExtensionProperties> exts(ne);
    if (enum_ext(pd, nullptr, &ne, exts.data()) != VK_SUCCESS) return 0;
    const bool has_info = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& x) {
        return std::strcmp(x.extensionName, "VK_AMD_shader_info") == 0;
    });
    if (!has_info) return 0;

    uint32_t nq = 0;
    get_qfp(pd, &nq, nullptr);
    if (!nq) return 0;
    std::vector<VkQueueFamilyProperties> qf(nq);
    get_qfp(pd, &nq, qf.data());
    uint32_t qfam = 0;
    for (uint32_t i = 0; i < nq; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* ext_name = "VK_AMD_shader_info";
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &ext_name;

    VkDevice dev = VK_NULL_HANDLE;
    if (create_dev(pd, &dci, nullptr, &dev) != VK_SUCCESS || !dev) return 0;

    using PFN_vkGetShaderInfoAMD_t = VkResult(VKAPI_PTR*)(VkDevice, VkPipeline, VkShaderStageFlagBits, int, size_t*, void*);
    auto destroy_dev = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(dev, "vkDestroyDevice"));
    auto create_sm = reinterpret_cast<PFN_vkCreateShaderModule>(gdpa(dev, "vkCreateShaderModule"));
    auto destroy_sm = reinterpret_cast<PFN_vkDestroyShaderModule>(gdpa(dev, "vkDestroyShaderModule"));
    auto create_pl = reinterpret_cast<PFN_vkCreatePipelineLayout>(gdpa(dev, "vkCreatePipelineLayout"));
    auto destroy_pl = reinterpret_cast<PFN_vkDestroyPipelineLayout>(gdpa(dev, "vkDestroyPipelineLayout"));
    auto create_cp = reinterpret_cast<PFN_vkCreateComputePipelines>(gdpa(dev, "vkCreateComputePipelines"));
    auto destroy_cp = reinterpret_cast<PFN_vkDestroyPipeline>(gdpa(dev, "vkDestroyPipeline"));
    auto get_info = reinterpret_cast<PFN_vkGetShaderInfoAMD_t>(gdpa(dev, "vkGetShaderInfoAMD"));

    uint32_t mach = 0;
    if (create_sm && destroy_sm && create_pl && destroy_pl && create_cp && destroy_cp && get_info) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sizeof(kProbeSpv);
        smci.pCode = kProbeSpv;
        VkShaderModule sm = VK_NULL_HANDLE;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (create_sm(dev, &smci, nullptr, &sm) == VK_SUCCESS && sm &&
            create_pl(dev, &plci, nullptr, &pl) == VK_SUCCESS && pl) {
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = sm;
            cpi.stage.pName = "main";
            cpi.layout = pl;
            if (create_cp(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe) == VK_SUCCESS && pipe) {
                size_t sz = 0;
                constexpr int kShaderInfoTypeBinaryAmd = 1;
                if (get_info(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, kShaderInfoTypeBinaryAmd, &sz, nullptr) == VK_SUCCESS && sz >= 64) {
                    std::vector<uint8_t> bin(sz);
                    if (get_info(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, kShaderInfoTypeBinaryAmd, &sz, bin.data()) == VK_SUCCESS) {
                        mach = onr::arch::parse_elf_mach(bin.data(), sz);
                    }
                }
                destroy_cp(dev, pipe, nullptr);
            }
        }
        if (pl) destroy_pl(dev, pl, nullptr);
        if (sm) destroy_sm(dev, sm, nullptr);
    }
    if (destroy_dev) destroy_dev(dev, nullptr);
    return mach;
}

bool ask_amd_icd(HMODULE icd, const uint8_t* luid, const uint8_t* uuid,
                 TrustedVkIdentity& out_id) {
    if (auto neg = reinterpret_cast<VkResult(VKAPI_PTR*)(uint32_t*)>(
            reinterpret_cast<void*>(GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion")))) {
        uint32_t ver = 5;
        neg(&ver);
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr")));
    auto create_inst = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
    if (!create_inst) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "openrr real-props";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (create_inst(&ici, nullptr, &inst) != VK_SUCCESS || !inst) return false;
    auto destroy_inst = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    auto enum_devs = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(inst, "vkGetPhysicalDeviceProperties2"));
    bool found = false;
    uint32_t n = 0;
    if (enum_devs && get_props2 && enum_devs(inst, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devs(n);
        if (enum_devs(inst, &n, devs.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < n && !found; ++i) {
                VkPhysicalDeviceDriverProperties d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceIDProperties di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &d};
                VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &di};
                get_props2(devs[i], &p);
                const bool luid_ok = luid && di.deviceLUIDValid && std::memcmp(di.deviceLUID, luid, VK_LUID_SIZE) == 0;
                const bool uuid_ok = !luid_ok && uuid && std::memcmp(di.deviceUUID, uuid, VK_UUID_SIZE) == 0;
                if (luid_ok || uuid_ok) {
                    out_id = {};
                    out_id.name = p.properties.deviceName;
                    out_id.vendor = p.properties.vendorID;
                    out_id.device = p.properties.deviceID;
                    out_id.driver_version = p.properties.driverVersion;
                    out_id.driver_info = d.driverInfo[0] ? d.driverInfo : d.driverName;
                    out_id.driver_id = uint32_t(d.driverID);
                    out_id.is_radv = d.driverID == VK_DRIVER_ID_MESA_RADV;
                    out_id.elf_mach = probe_icd_elf_mach(gipa, inst, devs[i]);
                    out_id.arch = out_id.elf_mach
                                      ? onr::arch::arch_from_mach(out_id.elf_mach)
                                      : onr::arch::arch_from_pci(out_id.vendor, out_id.device);
                    out_id.verified_icd = true;
                    found = true;
                }
            }
        }
    }
    if (destroy_inst) destroy_inst(inst, nullptr);
    return found;
}

std::filesystem::path extract_icd_library_from_json(const std::filesystem::path& json_path) {
    std::ifstream f(json_path, std::ios::binary);
    if (!f) return {};
    std::string body((std::istreambuf_iterator<char>(f)), {});
    const size_t k = body.find("\"library_path\"");
    if (k == std::string::npos) return {};
    const size_t c = body.find(':', k + 14);
    const size_t q1 = c == std::string::npos ? std::string::npos : body.find('"', c + 1);
    if (q1 == std::string::npos) return {};
    std::string raw;
    for (size_t i = q1 + 1; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
            raw.push_back(body[++i]);
        } else if (body[i] == '"') {
            break;
        } else {
            raw.push_back(body[i]);
        }
    }
    if (raw.empty()) return {};
    std::filesystem::path lib = u8_to_path(raw);
    if (lib.is_relative()) lib = json_path.parent_path() / lib;
    return lib.lexically_normal();
}

void push_unique_icd_module(std::vector<HMODULE>& out, const std::filesystem::path& dll_path) {
    if (dll_path.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dll_path, ec)) return;
    HMODULE m = GetModuleHandleW(dll_path.c_str());
    if (!m) m = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) m = LoadLibraryW(dll_path.c_str());
    if (!m || !GetProcAddress(m, "vk_icdGetInstanceProcAddr")) return;
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
}

void collect_amd_icds(std::vector<HMODULE>& icds) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me)) {
            if (_wcsnicmp(me.szModule, L"amdvlk", 6) == 0 && GetProcAddress(me.hModule, "vk_icdGetInstanceProcAddr")) {
                if (std::find(icds.begin(), icds.end(), me.hModule) == icds.end()) icds.push_back(me.hModule);
            }
        }
        CloseHandle(snap);
    }

    // Check Display Class registry keys (works even before vulkan-1 loads amdvlk64.dll into our process).
    HKEY cls = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &cls) == ERROR_SUCCESS) {
        for (DWORD idx = 0; idx < 64; ++idx) {
            wchar_t sub[64]{};
            DWORD sub_len = 64;
            if (RegEnumKeyExW(cls, idx, sub, &sub_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (sub[0] < L'0' || sub[0] > L'9') continue;
            HKEY hk = nullptr;
            if (RegOpenKeyExW(cls, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
            for (const wchar_t* val_name : {L"VulkanDriverName", L"OpenGLDriverName", L"UserModeDriverName"}) {
                wchar_t buf[4096]{};
                DWORD bytes = sizeof(buf) - sizeof(wchar_t);
                DWORD type = 0;
                if (RegQueryValueExW(hk, val_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes) == ERROR_SUCCESS) {
                    for (const wchar_t* p = buf; *p; p += std::wcslen(p) + 1) {
                        std::filesystem::path jp(p);
                        std::wstring lower = jp.filename().wstring();
                        for (auto& ch : lower) ch = wchar_t(std::towlower(ch));
                        if (jp.extension() == L".json" &&
                            (lower.find(L"amd") != std::wstring::npos || lower.find(L"ati") != std::wstring::npos)) {
                            push_unique_icd_module(icds, extract_icd_library_from_json(jp));
                        } else if (lower.find(L"amdxc") != std::wstring::npos || lower.find(L"atiogl") != std::wstring::npos ||
                                   lower.find(L"amdvlk") != std::wstring::npos) {
                            push_unique_icd_module(icds, jp.parent_path() / L"amdvlk64.dll");
                        }
                        if (type != REG_MULTI_SZ) break;
                    }
                }
            }
            RegCloseKey(hk);
        }
        RegCloseKey(cls);
    }

    // Check Khronos Vulkan Drivers registry registration.
    HKEY kdrv = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_READ, &kdrv) == ERROR_SUCCESS) {
        for (DWORD idx = 0;; ++idx) {
            wchar_t name[2048]{};
            DWORD name_len = 2048;
            if (RegEnumValueW(kdrv, idx, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::filesystem::path jp(name);
            std::wstring lower = jp.wstring();
            for (auto& ch : lower) ch = wchar_t(std::towlower(ch));
            if (lower.find(L"amd") != std::wstring::npos || lower.find(L"ati") != std::wstring::npos) {
                push_unique_icd_module(icds, extract_icd_library_from_json(jp));
            }
        }
        RegCloseKey(kdrv);
    }

    if (HMODULE sys = LoadLibraryExW(L"amdvlk64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (GetProcAddress(sys, "vk_icdGetInstanceProcAddr") &&
            std::find(icds.begin(), icds.end(), sys) == icds.end()) {
            icds.push_back(sys);
        }
    }
}

bool query_amd_icd_by_id(const uint8_t* luid, const uint8_t* uuid,
                         TrustedVkIdentity& out_id, bool bypass_cache) {
    uint64_t luid_key = 0;
    if (luid) std::memcpy(&luid_key, luid, sizeof(luid_key));
    if (luid && !bypass_cache) {
        std::lock_guard<std::mutex> lock(g_real_props_m);
        auto it = g_real_props_cache.find(luid_key);
        if (it != g_real_props_cache.end() && it->second.id.arch != onr::arch::GpuArch::Unknown) {
            out_id = it->second.id;
            return true;
        }
    }
    std::vector<HMODULE> icds;
    collect_amd_icds(icds);
    for (HMODULE icd : icds) {
        if (ask_amd_icd(icd, luid, uuid, out_id)) {
            if (luid && out_id.verified_icd && out_id.arch != onr::arch::GpuArch::Unknown) {
                std::lock_guard<std::mutex> lock(g_real_props_m);
                g_real_props_cache[luid_key] = CachedAmdIdentity{out_id};
            }
            return true;
        }
    }
    return false;
}
#endif

bool has_amd_marker_ext(const std::vector<VkExtensionProperties>& exts) {
    for (const auto& x : exts) {
        if (!std::strcmp(x.extensionName, "VK_AMD_shader_core_properties") ||
            !std::strcmp(x.extensionName, "VK_AMD_gcn_shader") ||
            !std::strcmp(x.extensionName, "VK_AMD_buffer_marker")) {
            return true;
        }
    }
    return false;
}

TrustedVkIdentity identify_vk_physical_device(VkPhysicalDevice pd, bool* out_luid_valid,
                                                uint8_t out_luid[VK_LUID_SIZE]) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &id};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    if (out_luid_valid) *out_luid_valid = id.deviceLUIDValid != VK_FALSE;
    if (out_luid && id.deviceLUIDValid) std::memcpy(out_luid, id.deviceLUID, VK_LUID_SIZE);

    const bool spoof_sim =
#ifdef OPENRR_TEST_HOOKS
        env_flag_on("ONR_TEST_SPOOF_LOADER");
#else
        false;
#endif
    const bool radv_sim =
#ifdef OPENRR_TEST_HOOKS
        env_flag_on("ONR_TEST_SIMULATE_RADV");
#else
        false;
#endif
    if (spoof_sim) {
        p2.properties.vendorID = 0x10de;
        p2.properties.deviceID = 0x2684;
        std::snprintf(p2.properties.deviceName, sizeof(p2.properties.deviceName), "NVIDIA GeForce RTX 4090");
        drv.driverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
        std::snprintf(drv.driverName, sizeof(drv.driverName), "NVIDIA");
        std::snprintf(drv.driverInfo, sizeof(drv.driverInfo), "566.36 (ONR_TEST_SPOOF_LOADER)");
    }

#ifdef _WIN32
    TrustedVkIdentity icd_id{};
    const uint8_t* luid_ptr = id.deviceLUIDValid ? id.deviceLUID : nullptr;
    if (query_amd_icd_by_id(luid_ptr, id.deviceUUID, icd_id, spoof_sim)) {
        if (radv_sim) {
            icd_id.driver_id = VK_DRIVER_ID_MESA_RADV;
            icd_id.is_radv = true;
            icd_id.driver_info = "Mesa 26.3.0 (ONR_TEST_SIMULATE_RADV)";
        }
        return icd_id;
    }
#endif

    TrustedVkIdentity fallback{};
    fallback.name = p2.properties.deviceName;
    fallback.vendor = p2.properties.vendorID;
    fallback.device = p2.properties.deviceID;
    fallback.driver_version = p2.properties.driverVersion;
    fallback.driver_info = drv.driverInfo[0] ? drv.driverInfo : drv.driverName;
    fallback.driver_id = uint32_t(drv.driverID);
    fallback.is_radv = radv_sim || drv.driverID == VK_DRIVER_ID_MESA_RADV;
    if (radv_sim) {
        fallback.driver_id = VK_DRIVER_ID_MESA_RADV;
        fallback.driver_info = "Mesa 26.3.0 (ONR_TEST_SIMULATE_RADV)";
    }
    if (fallback.vendor == 0x1002) {
        fallback.arch = onr::arch::arch_from_pci(fallback.vendor, fallback.device);
        if (fallback.is_radv && fallback.arch == onr::arch::GpuArch::Unknown) {
            const std::string tag = fallback.name + " " + fallback.driver_info;
            if (icontains(tag, "gfx1200")) { fallback.arch = onr::arch::GpuArch::Rdna4; fallback.elf_mach = 0x48; }
            else if (icontains(tag, "gfx1201")) { fallback.arch = onr::arch::GpuArch::Rdna4; fallback.elf_mach = 0x4e; }
            else if (icontains(tag, "rdna4") || icontains(tag, "rx 9070") || icontains(tag, "rx 9060") || icontains(tag, "r9700")) {
                fallback.arch = onr::arch::GpuArch::Rdna4;
            } else if (icontains(tag, "gfx11") || icontains(tag, "rx 7")) {
                fallback.arch = onr::arch::GpuArch::Rdna3;
            }
        }
    }
    return fallback;
}

bool query_real_vk_props(VkPhysicalDevice pd, VkPhysicalDeviceProperties& out_props,
                         VkPhysicalDeviceDriverProperties& out_drv,
                         VkPhysicalDeviceIDProperties* out_id) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    void* drv_next = out_drv.pNext;
    out_drv = VkPhysicalDeviceDriverProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &id};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &out_drv};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    out_props = p2.properties;
    out_drv.pNext = drv_next;
    if (out_id) *out_id = id;

    const TrustedVkIdentity tid = identify_vk_physical_device(pd);
    out_props.vendorID = tid.vendor;
    out_props.deviceID = tid.device;
    out_props.driverVersion = tid.driver_version;
    if (!tid.name.empty()) {
        std::snprintf(out_props.deviceName, sizeof(out_props.deviceName), "%s", tid.name.c_str());
    }
    if (!tid.driver_info.empty()) {
        std::snprintf(out_drv.driverInfo, sizeof(out_drv.driverInfo), "%s", tid.driver_info.c_str());
    }
    return tid.vendor == 0x1002;
}

VkPhysicalDevice pick_vk_physical_device(const std::vector<VkPhysicalDevice>& devs, const uint8_t* luid,
                                         std::string* out_name, uint32_t* out_api_version, bool* out_is_amd) {
    if (out_name) out_name->clear();
    if (out_api_version) *out_api_version = 0;
    if (out_is_amd) *out_is_amd = false;
    if (devs.empty()) return VK_NULL_HANDLE;

    const bool spoof_sim =
#ifdef OPENRR_TEST_HOOKS
        env_flag_on("ONR_TEST_SPOOF_LOADER");
#else
        false;
#endif
    const char* env_gpu_name = std::getenv("ONR_GPU_NAME");
    const std::string want_name = env_gpu_name ? env_gpu_name : "";

    struct Cand {
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        std::string name;
        uint32_t api_version = 0;
        VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        bool luid_valid = false;
        bool luid_match = false;
        bool is_amd = false;
        bool is_radv = false;
        bool has_cm = false;
        bool has_f8_cm = false;
        onr::arch::GpuArch arch = onr::arch::GpuArch::Unknown;
    };
    std::vector<Cand> cands;
    cands.reserve(devs.size());
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties p_props{};
        vkGetPhysicalDeviceProperties(pd, &p_props);
        bool luid_valid = false;
        uint8_t dev_luid[VK_LUID_SIZE]{};
        const TrustedVkIdentity tid = identify_vk_physical_device(pd, &luid_valid, dev_luid);
        const bool luid_match = luid && luid_valid && !std::memcmp(dev_luid, luid, VK_LUID_SIZE);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        if (ne) vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        bool has_cm = false, has_f8 = false;
        for (const auto& x : exts) {
            if (!std::strcmp(x.extensionName, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) has_cm = true;
            else if (!std::strcmp(x.extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) has_f8 = true;
        }
        const bool is_amd = (tid.vendor == 0x1002) ||
                            (tid.arch != onr::arch::GpuArch::Unknown) ||
                            (!spoof_sim && has_amd_marker_ext(exts));
        cands.push_back(Cand{pd, tid.name.empty() ? p_props.deviceName : tid.name, p_props.apiVersion,
                             p_props.deviceType, luid_valid, luid_match, is_amd, tid.is_radv,
                             has_cm, has_cm && has_f8, tid.arch});
    }

    const Cand* chosen = nullptr;
    if (luid) {
        for (const auto& c : cands) {
            if (c.luid_match) { chosen = &c; break; }
        }
        // Linux Wine/Proton + Mesa RADV does not always expose a Win32 adapter LUID matching vkd3d-proton;
        // fall back to AMD priority selection only when RADV lacks a valid LUID, keeping strict LUID
        // matching everywhere a Windows driver supplies valid LUIDs.
        const bool has_radv_without_luid = std::any_of(cands.begin(), cands.end(), [](const Cand& c) {
            return c.is_amd && c.is_radv && !c.luid_valid;
        });
        if (!chosen && !has_radv_without_luid) return VK_NULL_HANDLE;
    }
    if (!chosen) {
        if (!want_name.empty()) {
            for (const auto& c : cands) {
                if (icontains(c.name, want_name)) { chosen = &c; break; }
            }
        }
        // 1. Discrete GPU with AMD identity and RDNA4 (or FP8+coopmat)
        if (!chosen) {
            for (const auto& c : cands) {
                if (c.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && c.is_amd &&
                    (c.arch == onr::arch::GpuArch::Rdna4 || c.has_f8_cm)) {
                    chosen = &c; break;
                }
            }
        }
        // 2. Discrete GPU with AMD identity and cooperative matrix (RDNA3 discrete)
        if (!chosen) {
            for (const auto& c : cands) {
                if (c.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && c.is_amd && c.has_cm) { chosen = &c; break; }
            }
        }
        // 3. Discrete GPU with AMD identity
        if (!chosen) {
            for (const auto& c : cands) {
                if (c.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && c.is_amd) { chosen = &c; break; }
            }
        }
        // 4. Integrated/APU AMD GPU with cooperative matrix (e.g. Radeon 780M / 890M / 8060S)
        if (!chosen) {
            for (const auto& c : cands) {
                if (c.is_amd && c.has_cm) { chosen = &c; break; }
            }
        }
        // 5. Any remaining AMD device before non-AMD devices
        if (!chosen) {
            for (const auto& c : cands) {
                if (c.is_amd) { chosen = &c; break; }
            }
        }
        // 6. Fallback to first enumerated device only when no AMD candidate matched and no LUID was requested
        if (!chosen && !luid) chosen = &cands[0];
    }

    if (!chosen) return VK_NULL_HANDLE;
    if (out_name) *out_name = chosen->name;
    if (out_api_version) *out_api_version = chosen->api_version;
    if (out_is_amd) *out_is_amd = chosen->is_amd;
    return chosen->pd;
}

static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        char b[256];
        snprintf(b, sizeof b, "%s failed (%d)", what, int(r));
        throw std::runtime_error(b);
    }
}

static uint32_t probe_live_device_elf_mach(VkDevice dev, bool has_shader_info, bool has_pbin) {
    if (!dev || (!has_shader_info && !has_pbin)) return 0;
#ifndef _WIN32
    (void)dev;
    return 0;
#else
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kProbeSpv);
    smci.pCode = kProbeSpv;
    VkShaderModule sm = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreateShaderModule(dev, &smci, nullptr, &sm) != VK_SUCCESS || !sm ||
        vkCreatePipelineLayout(dev, &plci, nullptr, &pl) != VK_SUCCESS || !pl) {
        if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
        if (sm) vkDestroyShaderModule(dev, sm, nullptr);
        return 0;
    }

    uint32_t mach = 0;
    if (has_shader_info) {
        using PFN_vkGetShaderInfoAMD_t = VkResult(VKAPI_PTR*)(VkDevice, VkPipeline, VkShaderStageFlagBits, int, size_t*, void*);
        auto get_info = reinterpret_cast<PFN_vkGetShaderInfoAMD_t>(vkGetDeviceProcAddr(dev, "vkGetShaderInfoAMD"));
        if (get_info) {
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = sm;
            cpi.stage.pName = "main";
            cpi.layout = pl;
            VkPipeline pipe = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe) == VK_SUCCESS && pipe) {
                size_t sz = 0;
                constexpr int kShaderInfoTypeBinaryAmd = 1;
                if (get_info(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, kShaderInfoTypeBinaryAmd, &sz, nullptr) == VK_SUCCESS && sz >= 64) {
                    std::vector<uint8_t> bin(sz);
                    if (get_info(dev, pipe, VK_SHADER_STAGE_COMPUTE_BIT, kShaderInfoTypeBinaryAmd, &sz, bin.data()) == VK_SUCCESS) {
                        mach = onr::arch::parse_elf_mach(bin.data(), sz);
                    }
                }
                vkDestroyPipeline(dev, pipe, nullptr);
            }
        }
    }

    if (!onr::arch::aco_target_for_mach(mach) && has_pbin) {
        detail_pbin::PbinProcs fn;
        if (fn.load(dev)) {
            detail_pbin::CapturedPipe cap(dev, fn);
            VkPipelineCreateFlags2CreateInfoKHR f2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
            f2.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &f2};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpi.stage.module = sm;
            cpi.stage.pName = "main";
            cpi.layout = pl;
            if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &cap.pipe) == VK_SUCCESS && cap.pipe) {
                cap.captured = true;
                detail_pbin::BinaryList bins(dev, fn);
                VkPipelineBinaryCreateInfoKHR bi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
                bi.pipeline = cap.pipe;
                VkPipelineBinaryHandlesInfoKHR hi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
                if (fn.create(dev, &bi, nullptr, &hi) == VK_SUCCESS && hi.pipelineBinaryCount == 1) {
                    bins.handles.resize(1);
                    hi.pPipelineBinaries = bins.handles.data();
                    if (fn.create(dev, &bi, nullptr, &hi) == VK_SUCCESS) {
                        VkPipelineBinaryDataInfoKHR di{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR};
                        di.pipelineBinary = bins.handles[0];
                        VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                        size_t bytes = 0;
                        if (fn.data(dev, &di, &key, &bytes, nullptr) == VK_SUCCESS && bytes >= 64 && bytes <= 64 * 1024 * 1024) {
                            std::vector<uint8_t> elf(bytes);
                            if (fn.data(dev, &di, &key, &bytes, elf.data()) == VK_SUCCESS) {
                                const uint32_t pbin_mach = onr::arch::parse_elf_mach(elf.data(), bytes);
                                if (pbin_mach) mach = pbin_mach;
                            }
                        }
                    }
                }
            }
        }
    }

    vkDestroyPipelineLayout(dev, pl, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);
    return mach;
#endif
}

static constexpr VkDeviceSize kStaging = 64ull << 20;

// every Device adopting a game's device shares the one queue its setup added (one per feature): VkQueue needs
// external synchronization, so their one-shot submits take turns
static std::mutex g_queue_m;

// a one-shot submit on the runtime's queue, waited for; a device that stops answering fails it instead of hanging
static void submit_wait(VkDevice dev, VkQueue q, VkCommandBuffer cmd, VkFence fence,
                        const std::function<void()>& lock_fn = {},
                        const std::function<void()>& unlock_fn = {}) {
    std::lock_guard l(g_queue_m);
    struct UnlockScope {
        const std::function<void()>& u;
        ~UnlockScope() { if (u) u(); }
    } unlock{unlock_fn};
    if (lock_fn) lock_fn();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit");
    const VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (r == VK_SUCCESS) vkResetFences(dev, 1, &fence);
    check(r, "vkWaitForFences");
}


Device::Device() = default;

Device::~Device() {
    if (!dev_) return;
    aco_state_.reset();
    // hosted: the host's device is idle by now (the runtime's Destroy contract); everything goes at once
    const bool hosted = std::exchange(hosted_, false);
    if (!hosted) vkDeviceWaitIdle(dev_);
    sweep(true);
    destroy(staging_);
    if (qpool_) vkDestroyQueryPool(dev_, qpool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (d3d_layout_) vkDestroyPipelineLayout(dev_, d3d_layout_, nullptr);
    if (d3d_set_layout_) vkDestroyDescriptorSetLayout(dev_, d3d_set_layout_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (vpool_) vkDestroyDescriptorPool(dev_, vpool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (fence_) vkDestroyFence(dev_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    host_queue_lock_ = {};
    host_queue_unlock_ = {};
    if (hosted) return;
    vk_note_device_destroyed(dev_);
    vkDestroyDevice(dev_, nullptr);
    if (inst_) vkDestroyInstance(inst_, nullptr);
}

bool Device::init(std::string& err, const DeviceOptions& opt) {
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "openrr";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &inst_), "vkCreateInstance");

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst_, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst_, &n, devs.data());
        phys_ = pick_vk_physical_device(devs, opt.luid);
        if (!phys_) throw std::runtime_error(opt.luid ? "no Vulkan device with the adapter's LUID" : "no Vulkan device");

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        const TrustedVkIdentity tid = identify_vk_physical_device(phys_);
        info_.name = tid.name.empty() ? props.deviceName : tid.name;
        info_.vendor = tid.vendor ? tid.vendor : props.vendorID;
        info_.device = tid.device ? tid.device : props.deviceID;
        info_.driver_version = tid.driver_version ? tid.driver_version : props.driverVersion;
        info_.driver_info = tid.driver_info;
        info_.arch = tid.arch;
        info_.elf_mach = tid.elf_mach;
        info_.driver_id = tid.driver_id;
        info_.is_radv = tid.is_radv;
        ts_period_ns_ = props.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(phys_, &memp_);

        // features we use
        VkPhysicalDevicePipelineBinaryFeaturesKHR pbin{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
        VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &pe};
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f8};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &cm};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};

        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, exts.data());
        if (info_.vendor != 0x1002 && has_amd_marker_ext(exts)) info_.vendor = 0x1002;
        auto has = [&](const char* e) {
            return std::any_of(exts.begin(), exts.end(), [&](auto& x) { return !strcmp(x.extensionName, e); });
        };
        std::vector<const char*> want;
        const bool has_cm = has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        const bool has_f8 = has(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        if (has_cm) want.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        if (has_f8) want.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        const bool calibrated = has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        if (calibrated) want.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        stats_ = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        if (stats_) want.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        else f8.pNext = nullptr;
        if (!has_f8) cm.pNext = stats_ ? &pe : nullptr;
        if (!has_cm) v13.pNext = nullptr;
        // aliased shared blocks (vector views of the LDS of the network kernels)
        VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        const bool has_desc = opt.descriptors && has(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        if (has_desc) {
            want.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
            dbf.pNext = v11.pNext;
            v11.pNext = &dbf;
        }
        if (opt.interop) {
            for (const char* e : {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"})
                if (has(e)) want.push_back(e);
                else throw std::runtime_error(std::string("device lacks ") + e);
        }
        if (has(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME)) {
            want.push_back(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
            wl.pNext = v11.pNext;
            v11.pNext = &wl;
        }
        const bool has_shader_info = has("VK_AMD_shader_info");
        if (has_shader_info) want.push_back("VK_AMD_shader_info");
        const bool try_pbin = has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        if (try_pbin) {
            m5.pNext = v11.pNext;
            pbin.pNext = &m5;
            v11.pNext = &pbin;
        }
        vkGetPhysicalDeviceFeatures2(phys_, &f2);
        const bool pbin_enabled = try_pbin && pbin.pipelineBinaries && m5.maintenance5;
        if (pbin_enabled) {
            want.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            want.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
        } else if (try_pbin) {
            v11.pNext = m5.pNext;
        }
        info_.coopmat = has_cm && cm.cooperativeMatrix;
        info_.fp8_coopmat = has_f8 && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
        info_.fp64 = f2.features.shaderFloat64;
        info_.int64_atomics = v12.shaderBufferInt64Atomics;
        host_query_reset_ = v12.hostQueryReset != VK_FALSE;
        if (!v12.bufferDeviceAddress || !v12.shaderFloat16 || !v11.storageBuffer16BitAccess || !f2.features.shaderInt64)
            throw std::runtime_error("device lacks bufferDeviceAddress / float16 / 16-bit storage / int64");
        if (opt.interop && !v12.timelineSemaphore) throw std::runtime_error("device lacks timeline semaphores");

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, qf.data());
        qfam_ = UINT32_MAX;
        for (uint32_t i = 0; i < nq; ++i)
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qfam_ = i; break; }
        if (qfam_ == UINT32_MAX) throw std::runtime_error("no graphics+compute queue");
        float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(want.size());
        dci.ppEnabledExtensionNames = want.data();
        check(vkCreateDevice(phys_, &dci, nullptr, &dev_), "vkCreateDevice");
        vk_set_device_pipeline_binary(dev_, pbin_enabled);
        if ((info_.arch == onr::arch::GpuArch::Rdna4 || info_.fp8_coopmat) && !onr::arch::aco_target_for_mach(info_.elf_mach)) {
            const uint32_t live_mach = probe_live_device_elf_mach(dev_, has_shader_info, pbin_enabled);
            if (live_mach) {
                info_.elf_mach = live_mach;
                const auto live_arch = onr::arch::arch_from_mach(live_mach);
                if (live_arch != onr::arch::GpuArch::Unknown) info_.arch = live_arch;
            }
        }
        vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
        if (calibrated) calibrated_ = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(dev_, "vkGetCalibratedTimestampsEXT"));
        if (opt.descriptors && dbf.descriptorBuffer)
            get_descriptor_ = reinterpret_cast<PFN_vkGetDescriptorEXT>(vkGetDeviceProcAddr(dev_, "vkGetDescriptorEXT"));

        setup_common();
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

// pools, layouts, the query pool and the staging buffer, on dev_ / qfam_
void Device::setup_common() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam_;
    check(vkCreateCommandPool(dev_, &pci, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(dev_, &cai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(dev_, &fci, nullptr, &fence_), "vkCreateFence");

    VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTexSlots, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorBindingFlags bflags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 2;
    bfi.pBindingFlags = bflags;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi};
    dli.bindingCount = 2;
    dli.pBindings = binds;
    check(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    const uint32_t sets = hosted_ ? 64u : 1u;
    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kTexSlots * sets},
        {VK_DESCRIPTOR_TYPE_SAMPLER, sets},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    check(vkAllocateDescriptorSets(dev_, &dai, &set_), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &set_layout_;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(dev_, &lci, nullptr, &layout_), "vkCreatePipelineLayout");

    VkDescriptorSetLayoutBinding db[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_COMPUTE_BIT},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorBindingFlags dflags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo dfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    dfi.bindingCount = 2;
    dfi.pBindingFlags = dflags;
    VkDescriptorSetLayoutCreateInfo ddi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &dfi};
    ddi.bindingCount = 2;
    ddi.pBindings = db;
    check(vkCreateDescriptorSetLayout(dev_, &ddi, nullptr, &d3d_set_layout_), "vkCreateDescriptorSetLayout");
    lci.pSetLayouts = &d3d_set_layout_;
    check(vkCreatePipelineLayout(dev_, &lci, nullptr, &d3d_layout_), "vkCreatePipelineLayout");

    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = kMaxQueries;
    check(vkCreateQueryPool(dev_, &qpi, nullptr, &qpool_), "vkCreateQueryPool");
}

bool Device::adopt(const HostDevice& h, std::string& err) {
    try {
        hosted_ = true;
        phys_ = h.physical;
        dev_ = h.device;
        {
            std::lock_guard<std::mutex> lock(g_vk_track_m);
            g_destroyed_vk_devices.erase(dev_);
        }
        qfam_ = h.queue_family;
        host_queue_lock_ = h.queue_lock;
        host_queue_unlock_ = h.queue_unlock;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        const TrustedVkIdentity tid = identify_vk_physical_device(phys_);
        info_.name = tid.name.empty() ? props.deviceName : tid.name;
        info_.vendor = tid.vendor ? tid.vendor : props.vendorID;
        info_.device = tid.device ? tid.device : props.deviceID;
        info_.driver_version = tid.driver_version ? tid.driver_version : props.driverVersion;
        info_.driver_info = tid.driver_info;
        info_.arch = tid.arch;
        info_.elf_mach = tid.elf_mach;
        info_.driver_id = tid.driver_id;
        info_.is_radv = tid.is_radv;
        ts_period_ns_ = props.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(phys_, &memp_);
        bool has_f8 = false;
        {
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> exts(ne);
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, exts.data());
            if (info_.vendor != 0x1002 && has_amd_marker_ext(exts)) info_.vendor = 0x1002;
            for (auto& e : exts) {
                budget_ |= !strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                has_f8 |= !strcmp(e.extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
            }
        }
        // the host enabled these through PrepareVkDevice, which asked the GPU the same
        VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, has_f8 ? &f8 : nullptr};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &cm};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(phys_, &f2);
        info_.coopmat = cm.cooperativeMatrix;
        info_.fp8_coopmat = has_f8 && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
        info_.fp64 = f2.features.shaderFloat64;
        if ((info_.arch == onr::arch::GpuArch::Rdna4 || info_.fp8_coopmat) && !onr::arch::aco_target_for_mach(info_.elf_mach)) {
            const bool dev_pbin = vk_device_has_pipeline_binary(dev_);
            const uint32_t live_mach = probe_live_device_elf_mach(dev_, false, dev_pbin);
            if (live_mach) {
                info_.elf_mach = live_mach;
                const auto live_arch = onr::arch::arch_from_mach(live_mach);
                if (live_arch != onr::arch::GpuArch::Unknown) info_.arch = live_arch;
            }
        }
        host_query_reset_ = v12.hostQueryReset != VK_FALSE;
        if (!host_query_reset_)
            throw std::runtime_error("the host's device lacks hostQueryReset");
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
        for (uint32_t i = 0; i < nq; ++i) families_.push_back(i);
        vkGetDeviceQueue(dev_, h.queue_family, h.queue_index, &queue_);
        if (!queue_)
            throw std::runtime_error("the host's device has no queue " + std::to_string(h.queue_index) + " in family " +
                                     std::to_string(h.queue_family));
        setup_common();
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

void Device::bury(const Grave& g) {
    Grave item = g;
    for (const auto& q : active_queries_) item.wait_serials.push_back(q.serial);
    if (current_serial_) item.wait_serials.push_back(current_serial_);
    grave_.push_back(std::move(item));
    sweep(false);
}

void Device::sweep(bool all) {
    if (!all && dev_) {
        const bool dev_gone = vk_device_is_destroyed(dev_);
        std::erase_if(active_queries_, [&](QueryProof& q) {
            if (q.serial == current_serial_) return false;
            if (dev_gone) return true;
            if (vk_cmd_retired(q.cmd, q.cmd_epoch, q.cmd_pool, q.pool_epoch)) return true;
            if (binds_ - q.serial >= kViewSlots && !q.warned) {
                q.warned = true;
            }
            return false;
        });
    } else if (all) {
        active_queries_.clear();
    }
    size_t keep = 0;
    for (auto& g : grave_) {
        if (!all) {
            std::erase_if(g.wait_serials, [&](uint64_t s) {
                if (s == current_serial_) return false;
                return std::none_of(active_queries_.begin(), active_queries_.end(), [&](const QueryProof& q) { return q.serial == s; });
            });
            if (!g.wait_serials.empty()) {
                grave_[keep++] = std::move(g);
                continue;
            }
        }
        if (g.set) vkFreeDescriptorSets(dev_, dpool_, 1, &g.set);
        if (g.vset) vkFreeDescriptorSets(dev_, vpool_, 1, &g.vset);
        if (g.qpool) vkDestroyQueryPool(dev_, g.qpool, nullptr);
        if (g.view) vkDestroyImageView(dev_, g.view, nullptr);
        if (g.img) vkDestroyImage(dev_, g.img, nullptr);
        if (g.sampler) vkDestroySampler(dev_, g.sampler, nullptr);
        if (g.buf) vkDestroyBuffer(dev_, g.buf, nullptr);
        if (g.mem) vkFreeMemory(dev_, g.mem, nullptr);
        if (g.pipe) vkDestroyPipeline(dev_, g.pipe, nullptr);
        if (g.module) vkDestroyShaderModule(dev_, g.module, nullptr);
    }
    grave_.resize(keep);
}

bool Device::pool_retired(VkQueryPool q) {
    if (!q) return true;
    sweep(false);
    return std::none_of(active_queries_.begin(), active_queries_.end(), [&](const QueryProof& p) { return p.pool == q; });
}

bool Device::all_retired() {
    sweep(false);
    return grave_.empty() && active_queries_.empty();
}

bool Device::vram(uint64_t& used, uint64_t& budget) const {
    if (!budget_) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 m{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
    vkGetPhysicalDeviceMemoryProperties2(phys_, &m);
    used = budget = 0;
    for (uint32_t i = 0; i < m.memoryProperties.memoryHeapCount; ++i)
        if (m.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += b.heapUsage[i], budget += b.heapBudget[i];
    return true;
}

uint32_t Device::mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memp_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memp_.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no memory type");
}

Buffer Device::create_ex(VkDeviceSize size, VkMemoryPropertyFlags props, void** map) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<VkDeviceSize>(size, 16);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (hosted_ && families_.size() > 1) {
        bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bci.queueFamilyIndexCount = uint32_t(families_.size());
        bci.pQueueFamilyIndices = families_.data();
    }
    check(vkCreateBuffer(dev_, &bci, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    VkMemoryAllocateFlagsInfo afi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    afi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &afi};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type(req.memoryTypeBits, props);
    check(vkAllocateMemory(dev_, &mai, nullptr, &b.mem), "vkAllocateMemory");
    bytes_ += size;
    vkBindBufferMemory(dev_, b.buf, b.mem, 0);
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(dev_, &dai);
    if (map) check(vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, map), "vkMapMemory");
    return b;
}

Buffer Device::create(VkDeviceSize size) { return create_ex(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr); }

Buffer Device::create_host_coherent(VkDeviceSize size, void** map) {
    return create_ex(size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, map);
}

void Device::destroy(Buffer& b) {
    if (b.mem) bytes_ -= std::min(bytes_, uint64_t(b.size));
    if (hosted_ && (b.buf || b.mem)) {
        Grave g;
        g.buf = b.buf, g.mem = b.mem;
        bury(g);
        b = Buffer{};
        return;
    }
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = Buffer{};
}

void Device::one_shot_copy(VkBuffer src, VkBuffer dst, VkDeviceSize so, VkDeviceSize dof, VkDeviceSize size) {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkBufferCopy c{so, dof, size};
    vkCmdCopyBuffer(cmd_, src, dst, 1, &c);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_, host_queue_lock_, host_queue_unlock_);
}

void Device::upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off) {
    if (!size) return;
    const VkDeviceSize need = std::min(kStaging, std::max<VkDeviceSize>((size + 65535) & ~VkDeviceSize(65535), 4ull << 20));
    if (staging_.size < need) {
        destroy(staging_);
        staging_ = create_ex(need, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_map_);
    }
    for (VkDeviceSize done = 0; done < size;) {
        VkDeviceSize n = std::min(staging_.size, size - done);
        memcpy(staging_map_, static_cast<const char*>(src) + done, size_t(n));
        one_shot_copy(staging_.buf, b.buf, 0, off + done, n);
        done += n;
    }
}

void Device::download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off) {
    if (!size) return;
    const VkDeviceSize need = std::min(kStaging, std::max<VkDeviceSize>((size + 65535) & ~VkDeviceSize(65535), 4ull << 20));
    if (staging_.size < need) {
        destroy(staging_);
        staging_ = create_ex(need, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_map_);
    }
    for (VkDeviceSize done = 0; done < size;) {
        VkDeviceSize n = std::min(staging_.size, size - done);
        one_shot_copy(b.buf, staging_.buf, off + done, 0, n);
        memcpy(static_cast<char*>(dst) + done, staging_map_, size_t(n));
        done += n;
    }
}

void Device::begin_batch() {
    if (batch_ || current_serial_ != 0 || (own_cmd_ != VK_NULL_HANDLE && cmd_ != own_cmd_)) return;
    batch_ = true;
    batch_dirty_ = false;
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
}

void Device::end_batch() {
    if (!batch_) return;
    batch_ = false;
    vkEndCommandBuffer(cmd_);
    if (batch_dirty_) submit_wait(dev_, queue_, cmd_, fence_);
    batch_dirty_ = false;
}

void Device::fill(const Buffer& b, uint32_t value) {
    if (current_serial_ != 0 || (own_cmd_ != VK_NULL_HANDLE && cmd_ != own_cmd_)) {
        fill(b, 0, VK_WHOLE_SIZE, value);
        barrier_wide();
        return;
    }
    if (batch_) {
        vkCmdFillBuffer(cmd_, b.buf, 0, VK_WHOLE_SIZE, value);
        batch_dirty_ = true;
        return;
    }
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdFillBuffer(cmd_, b.buf, 0, VK_WHOLE_SIZE, value);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_, host_queue_lock_, host_queue_unlock_);
}

void Device::set_compiler(bool aco, bool strict, const std::string& cache_dir, const std::string& shader_dir) {
    aco_ = aco;
    aco_strict_ = strict;
    if (!cache_dir.empty()) cache_dir_ = cache_dir;
    if (!shader_dir.empty()) shader_dir_ = shader_dir;
    if (!aco_) {
        aco_state_.reset();
        aco_reason_.clear();
        return;
    }
    const bool dev_pbin = dev_ ? vk_device_has_pipeline_binary(dev_) : false;
    aco_state_ = std::make_unique<PipelineBinaryLoader>(dev_, info_.elf_mach, dev_pbin, cache_dir_, shader_dir_);
    aco_reason_ = aco_state_->ready() ? std::string() : aco_state_->reason();
}

void Device::set_compiler_log(std::function<void(const std::string&)> log) {
    aco_log_ = std::move(log);
}

bool Device::aco_ready() const {
    return aco_ && aco_state_ && aco_state_->ready();
}

uint32_t Device::aco_cache_hits() const {
    return aco_state_ ? aco_state_->cache_hits() : 0;
}

uint32_t Device::aco_cache_misses() const {
    return aco_state_ ? aco_state_->cache_misses() : 0;
}

Kernel Device::load(const std::string& path, uint32_t subgroup) {
    const std::filesystem::path fs_path = u8_to_path(path);
    std::ifstream f(fs_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    const std::string stem = path_to_u8(fs_path.stem());
    const uint64_t spv_hash_val = pal::hash(code.data(), code.size());
    const std::string spv_key = detail_pbin::hex64(spv_hash_val);
    auto log_compiler = [&](const std::string& message) {
        const std::string line = "[openrr compiler] " + stem + " spv=" + spv_key + " " + message + "\n";
#ifdef _WIN32
        OutputDebugStringA(line.c_str());
#endif
        std::fputs(line.c_str(), stderr);
        if (aco_log_) aco_log_(line.substr(0, line.size() - 1));
    };

    Kernel k;
    k.name = path;
    k.views = stem.rfind("d3d_", 0) == 0;
    const VkPipelineLayout lay = k.views ? d3d_layout_ : layout_;

    std::vector<uint32_t> hardened_spv;
    const uint32_t* spv_ptr = reinterpret_cast<const uint32_t*>(code.data());
    size_t spv_bytes_len = code.size();
    if (aco_) {
        try {
            if (!aco_state_) throw AcoFallbackError(aco_reason_.empty() ? "Vulkan ACO loader uninitialized" : aco_reason_);
            pal::Bytes spv_bytes(reinterpret_cast<const uint8_t*>(code.data()),
                                 reinterpret_cast<const uint8_t*>(code.data()) + code.size());
            const auto res = aco_state_->load(spv_bytes, path, subgroup, lay, pipeline_flags, k.views);
            k.pipe = res.pipe;
            k.aco = true;
            log_compiler("compiler=ACO backend=vulkan cache=" + std::string(res.cache_hit ? "hit" : "miss") +
                         " record=" + res.nrp_path + " code=" + detail_pbin::hex64(res.code_hash) +
                         " rsrc1=" + detail_pbin::hex32(res.rsrc1) + " scratch=" + std::to_string(res.scratch_bytes) +
                         " manifest=" + res.manifest_path);
            return k;
        } catch (const FatalVkError&) {
            throw;
        } catch (const std::exception& e) {
            throw AcoFallbackError(stem + ": required Vulkan ACO: " + e.what());
        }
    } else if (info_.is_radv) {
        uint32_t added_nc = 0;
        hardened_spv = harden_spirv_for_radv(reinterpret_cast<const uint8_t*>(code.data()), code.size(), stem, &added_nc);
        if (!hardened_spv.empty()) {
            spv_ptr = hardened_spv.data();
            spv_bytes_len = hardened_spv.size() * sizeof(uint32_t);
        }
        k.aco = true;
        log_compiler("compiler=ACO backend=vulkan-radv precision=" +
                     std::string(is_radv_native_precision_stem(stem) ? "native" : "global") +
                     " nocontract_added=" + std::to_string(added_nc));
    } else {
        log_compiler("compiler=LLPC backend=vulkan");
    }

    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = spv_bytes_len;
    sci.pCode = spv_ptr;
    check(vkCreateShaderModule(dev_, &sci, nullptr, &k.module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = k.module;
    cpi.stage.pName = "main";
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rss.requiredSubgroupSize = subgroup;
    if (subgroup) {
        cpi.stage.pNext = &rss;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    cpi.layout = lay;
    cpi.flags = pipeline_flags;
    if (stats_) cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    const char* isa = stats_ ? getenv("ONR_ISA") : nullptr;   // directory: dump the driver's internal representations
    if (isa) cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    check(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &k.pipe), "vkCreateComputePipelines");
    if (isa) {
        auto ir = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableInternalRepresentationsKHR"));
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = k.pipe;
        uint32_t n = 0;
        if (ir && ir(dev_, &ei, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPipelineExecutableInternalRepresentationKHR> rs(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            ir(dev_, &ei, &n, rs.data());
            std::vector<std::vector<char>> data(n);
            for (uint32_t i = 0; i < n; ++i) data[i].resize(rs[i].dataSize), rs[i].pData = data[i].data();
            ir(dev_, &ei, &n, rs.data());
            for (uint32_t i = 0; i < n; ++i) {
                std::ofstream o(u8_to_path(isa) / u8_to_path(stem + "." + std::to_string(i) + ".txt"), std::ios::binary);
                o << rs[i].name << '\n' << rs[i].description << '\n';
                o.write(data[i].data(), std::streamsize(data[i].size()));
            }
        }
    }
    return k;
}

std::vector<uint8_t> Device::pipeline_elf(const std::string& path, uint32_t subgroup) {
    if (!stats_) return {};
    const std::filesystem::path fs_path = u8_to_path(path);
    std::ifstream f(fs_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = code.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    check(vkCreateShaderModule(dev_, &sci, nullptr, &mod), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rss.requiredSubgroupSize = subgroup;
    if (subgroup) {
        cpi.stage.pNext = &rss;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    cpi.layout = path_to_u8(fs_path.stem()).rfind("d3d_", 0) == 0 ? d3d_layout_ : layout_;
    cpi.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
    std::vector<uint8_t> elf;
    if (r == VK_SUCCESS) {
        auto ir = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
            vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableInternalRepresentationsKHR"));
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipe;
        uint32_t n = 0;
        if (ir && ir(dev_, &ei, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPipelineExecutableInternalRepresentationKHR> rs(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            ir(dev_, &ei, &n, rs.data());
            std::vector<std::vector<uint8_t>> data(n);
            for (uint32_t i = 0; i < n; ++i) data[i].resize(rs[i].dataSize), rs[i].pData = data[i].data();
            ir(dev_, &ei, &n, rs.data());
            const uint8_t magic[] = {0x7f, 'E', 'L', 'F'};
            for (uint32_t i = 0; i < n && elf.empty(); ++i) {
                auto at = std::search(data[i].begin(), data[i].end(), magic, magic + 4);
                if (!strncmp(rs[i].name, "ELF", 3) && at != data[i].end()) elf.assign(at, data[i].end());
            }
        }
        vkDestroyPipeline(dev_, pipe, nullptr);
    }
    vkDestroyShaderModule(dev_, mod, nullptr);
    check(r, "vkCreateComputePipelines");
    return elf;
}

std::string Device::stats(const Kernel& k) {
    if (!stats_) return {};
    auto props = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableStatisticsKHR"));
    if (!props) return {};
    VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    ei.pipeline = k.pipe;
    uint32_t n = 0;
    props(dev_, &ei, &n, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> st(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    props(dev_, &ei, &n, st.data());
    std::string r;
    for (auto& s : st) {
        char b[160];
        if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) snprintf(b, sizeof b, "%s=%llu ", s.name, (unsigned long long)s.value.u64);
        else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) snprintf(b, sizeof b, "%s=%lld ", s.name, (long long)s.value.i64);
        else continue;
        r += b;
    }
    return r;
}

void Device::destroy(Kernel& k) {
    if (hosted_ && (k.pipe || k.module)) {
        Grave g;
        g.pipe = k.pipe, g.module = k.module;
        bury(g);
        k = Kernel{};
        return;
    }
    if (k.pipe) vkDestroyPipeline(dev_, k.pipe, nullptr);
    if (k.module) vkDestroyShaderModule(dev_, k.module, nullptr);
    k = Kernel{};
}

Image Device::create_image(uint32_t w, uint32_t h, VkFormat fmt, const Buffer& src) {
    Image im;
    im.w = w;
    im.h = h;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(dev_, &ici, nullptr, &im.img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(dev_, &mai, nullptr, &im.mem), "vkAllocateMemory");
    vkBindImageMemory(dev_, im.img, im.mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev_, &vci, nullptr, &im.view), "vkCreateImageView");

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im.img;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd_, &di);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, src.buf, im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cmd_, &di);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_, host_queue_lock_, host_queue_unlock_);
    return im;
}

Image Device::alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) {
    Image im;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (hosted_ && families_.size() > 1) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = uint32_t(families_.size());
        ici.pQueueFamilyIndices = families_.data();
    }
    if (vkCreateImage(dev_, &ici, nullptr, &im.img) != VK_SUCCESS) return {};
    VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay{};
    vkGetImageSubresourceLayout(dev_, im.img, &sr, &lay);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    // the buffer's rows must be the image's rows
    if (lay.offset != 0 || lay.rowPitch != VkDeviceSize(w) * bpp || req.size > b.size || vkBindImageMemory(dev_, im.img, b.mem, 0) != VK_SUCCESS) {
        vkDestroyImage(dev_, im.img, nullptr);
        return {};
    }
    im.w = w, im.h = h;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev_, &vci, nullptr, &im.view), "vkCreateImageView");
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im.img;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd_, &di);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_, host_queue_lock_, host_queue_unlock_);
    return im;
}

void Device::destroy(Image& i) {
    if (i.view) {
        for (auto& sl : slots_)
            if (sl.imageView == i.view) sl = {};
    }
    if (hosted_ && (i.img || i.view || i.mem)) {
        Grave g;
        g.img = i.img, g.view = i.view, g.mem = i.mem;
        bury(g);
        i = Image{};
        return;
    }
    if (i.view) vkDestroyImageView(dev_, i.view, nullptr);
    if (i.img) vkDestroyImage(dev_, i.img, nullptr);
    if (i.mem) vkFreeMemory(dev_, i.mem, nullptr);
    i = Image{};
}

VkSampler Device::sampler(bool linear, bool unnormalized) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.unnormalizedCoordinates = unnormalized;
    VkSampler r;
    check(vkCreateSampler(dev_, &s, nullptr, &r), "vkCreateSampler");
    return r;
}

bool Device::combined_descriptor(const Image& im, VkSampler s, uint8_t out[48]) {
    if (!get_descriptor_ || !im.view) return false;
    VkDescriptorImageInfo ii{s, im.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorGetInfoEXT gi{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    gi.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gi.data.pCombinedImageSampler = &ii;
    get_descriptor_(dev_, &gi, 48, out);
    return true;
}

void Device::destroy(VkSampler s) {
    if (!s) return;
    if (tex_smp_ == s) tex_smp_ = VK_NULL_HANDLE;
    if (hosted_) {
        Grave g;
        g.sampler = s;
        bury(g);
        return;
    }
    vkDestroySampler(dev_, s, nullptr);
}

void Device::set_texture(uint32_t slot, const Image& i, VkSampler s) {
    VkDescriptorImageInfo img_info{VK_NULL_HANDLE, i.view, VK_IMAGE_LAYOUT_GENERAL};
    VkSampler active_smp = s ? s : tex_smp_;
    if (s) tex_smp_ = s;
    // hosted: a set a recording of the game may still read is not written again; a fresh one takes the slots
    if (hosted_ && set_bound_) {
        VkDescriptorSet fresh = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &fresh), "vkAllocateDescriptorSets");
        Grave g;
        g.set = set_;
        bury(g);
        set_ = fresh;
        set_bound_ = false;
        for (uint32_t k = 0; k < kTexSlots; ++k) {
            if (!slots_[k].imageView || k == slot) continue;
            VkDescriptorImageInfo copy_info = slots_[k];
            copy_info.sampler = VK_NULL_HANDLE;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_;
            w.dstBinding = 0;
            w.dstArrayElement = k;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &copy_info;
            vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        }
    }
    if (slot < kTexSlots) slots_[slot] = img_info;
    VkDescriptorImageInfo smp_info{active_smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set_;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = slot;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &img_info;
    uint32_t write_count = 1;
    if (active_smp) {
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set_;
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &smp_info;
        write_count = 2;
    }
    vkUpdateDescriptorSets(dev_, write_count, writes, 0, nullptr);
}

void Device::reset_textures() {
    std::memset(slots_, 0, sizeof(slots_));
    tex_smp_ = VK_NULL_HANDLE;
    if (!dev_ || !dpool_ || !set_layout_) {
        set_bound_ = false;
        return;
    }
    if (set_bound_) {
        VkDescriptorSet fresh = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &fresh), "vkAllocateDescriptorSets");
        if (hosted_) {
            Grave g;
            g.set = set_;
            bury(g);
        } else if (set_) {
            vkFreeDescriptorSets(dev_, dpool_, 1, &set_);
        }
        set_ = fresh;
        set_bound_ = false;
    }
}

void Device::use_views(uint32_t slot) {
    if (!vpool_) {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kViewSlots * 2},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kViewSlots * 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = kViewSlots * 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = ps;
        check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &vpool_), "vkCreateDescriptorPool");
    }
    if (slot >= kViewSlots) {
        VkDescriptorSet fresh = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = vpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &d3d_set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &fresh), "vkAllocateDescriptorSets");
        Grave g;
        g.vset = fresh;
        bury(g);
        views_ = fresh;
        return;
    }
    if (!vsets_[slot]) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = vpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &d3d_set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &vsets_[slot]), "vkAllocateDescriptorSets");
    }
    views_ = vsets_[slot];
}

void Device::set_view(uint32_t index, VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, index < 16 ? layout : VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = views_;
    w.dstBinding = index < 16 ? 0 : 1;
    w.dstArrayElement = index < 16 ? index : index - 16;
    w.descriptorCount = 1;
    w.descriptorType = index < 16 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}

bool Device::calibrate(uint64_t& gpu, uint64_t& qpc) const {
    if (!calibrated_) return false;
    VkCalibratedTimestampInfoEXT ci[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
                                          {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT}};
    uint64_t t[2] = {}, dev = 0;
    if (calibrated_(dev_, 2, ci, t, &dev) != VK_SUCCESS) return false;
    gpu = t[0], qpc = t[1];
    return true;
}

void Device::begin() {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdResetQueryPool(cmd_, qpool_, 0, kMaxQueries);
    nquery_ = 0;
    shader_ = transfer_ = false;
}

void Device::dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy, uint32_t gz) {
    set_bound_ = true;
    if (transfer_) barrier_wide();
    VkPipelineLayout lay = k.views ? d3d_layout_ : layout_;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, k.views ? &views_ : &set_, 0, nullptr);
    if (push_size) vkCmdPushConstants(cmd_, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    const bool timed = per_dispatch_ && nquery_ + 2 <= kMaxQueries;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_NONE, qpool_, nquery_++);
    vkCmdDispatch(cmd_, gx, gy, gz);
    shader_ = true;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off) {
    set_bound_ = true;
    barrier_wide();   // the arguments were written by a shader
    VkPipelineLayout lay = k.views ? d3d_layout_ : layout_;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, k.views ? &views_ : &set_, 0, nullptr);
    if (push_size) vkCmdPushConstants(cmd_, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    const bool timed = per_dispatch_ && nquery_ + 2 <= kMaxQueries;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_NONE, qpool_, nquery_++);
    vkCmdDispatchIndirect(cmd_, args.buf, off);
    shader_ = true;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) {
    if (shader_) barrier_wide();   // a shader may still read what the fill overwrites
    transfer_ = true;
    vkCmdFillBuffer(cmd_, b.buf, off, size, value);
}

void Device::update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) {
    if (shader_) barrier_wide();
    transfer_ = true;
    vkCmdUpdateBuffer(cmd_, b.buf, off, size, data);
}

void Device::bind(VkCommandBuffer c, VkQueryPool q) {
    current_serial_ = ++binds_;
    if (hosted_) sweep(false);
    own_cmd_ = cmd_, own_qpool_ = qpool_;
    bound_cmd_epoch_ = 0;
    bound_cmd_pool_ = VK_NULL_HANDLE;
    bound_pool_epoch_ = 0;
    if (hosted_) bind_vk_cmd_epoch(dev_, c, bound_cmd_epoch_, bound_cmd_pool_, bound_pool_epoch_);
    if (hosted_ && (!q || !pool_retired(q))) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMaxQueries;
        VkQueryPool fresh = VK_NULL_HANDLE;
        check(vkCreateQueryPool(dev_, &qi, nullptr, &fresh), "vkCreateQueryPool");
        Grave g;
        g.qpool = fresh;
        bury(g);
        q = fresh;
    }
    cmd_ = c, qpool_ = q;
    vkCmdResetQueryPool(cmd_, qpool_, 0, kMaxQueries);
    nquery_ = 0;
    shader_ = transfer_ = false;
}

void Device::stamp() {
    if (nquery_ < kMaxQueries) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::unbind() {
    if (hosted_) {
        const VkQueryPool qp = (qpool_ && qpool_ != own_qpool_) ? qpool_ : VK_NULL_HANDLE;
        active_queries_.push_back({qp, nquery_ ? nquery_ - 1 : 0, current_serial_, cmd_, bound_cmd_epoch_, bound_cmd_pool_, bound_pool_epoch_, nquery_ > 0 && qp != VK_NULL_HANDLE, false});
    }
    current_serial_ = 0;
    cmd_ = own_cmd_, qpool_ = own_qpool_;
    own_cmd_ = VK_NULL_HANDLE, own_qpool_ = VK_NULL_HANDLE;
    bound_cmd_epoch_ = 0;
    bound_cmd_pool_ = VK_NULL_HANDLE;
    bound_pool_epoch_ = 0;
    shader_ = transfer_ = false;
}

// between kernels: compute to compute only (a wider destination also syncs the command processor); after a fill or
// an update the transfer side too
void Device::barrier() {
    if (transfer_) return barrier_wide();
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &di);
}

void Device::barrier_wide() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &di);
    transfer_ = shader_ = false;
}

std::vector<double> Device::submit() {
    barrier_wide();
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_, host_queue_lock_, host_queue_unlock_);
    std::vector<double> ms;
    if (nquery_) {
        std::vector<uint64_t> ts(nquery_);
        vkGetQueryPoolResults(dev_, qpool_, 0, nquery_, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        for (uint32_t i = 0; i + 1 < nquery_; i += 2) ms.push_back(double(ts[i + 1] - ts[i]) * ts_period_ns_ * 1e-6);
        span_ms_ = double(ts[nquery_ - 1] - ts[0]) * ts_period_ns_ * 1e-6;
    }
    return ms;
}

}  // namespace onr
