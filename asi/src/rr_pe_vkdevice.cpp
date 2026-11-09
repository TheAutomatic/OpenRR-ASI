// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_pe_vkdevice.hpp"
#include "rr_pe_log.hpp"
#include "opennr_rr_api.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" void rr_vk_load(void);

namespace openrr::pe {
void vulkan_entry_points() {
    rr_vk_load();
}
} // namespace openrr::pe

namespace openrr::pe::vkdevice {
namespace {

std::mutex g_mutex;
std::vector<DeviceInfo> g_devices;
std::map<VkQueue, std::unique_ptr<std::mutex>> g_queue_locks;
std::string g_gpu_name;
std::string g_driver_name;

void query_adapter_names(VkPhysicalDevice phys) {
    if (!phys) return;
    openrr::pe::vulkan_entry_points();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (p2.properties.deviceName[0]) g_gpu_name = p2.properties.deviceName;
    if (drv.driverInfo[0]) g_driver_name = drv.driverInfo;
    else if (drv.driverName[0]) g_driver_name = drv.driverName;
}

} // namespace

VkResult create_with_runtime(PFN_vkCreateDevice create, VkInstance instance, uint32_t api_version,
                             VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                             const VkAllocationCallbacks* allocator, VkDevice* out, DeviceInfo* out_info) {
    openrr::pe::vulkan_entry_points();
    query_adapter_names(physical);

    OnrRrApi5 api5{};
    const bool has_api5 = OnrRrGetApi(ONR_RR_ABI_VERSION_5, &api5.v4.v3.v2.v1) == ONR_RR_OK &&
                          api5.PrepareVkDevice && api5.FinishVkDevice;

    VkDeviceCreateInfo patched{};
    void* setup = nullptr;
    bool prepared = false;
    if (has_api5 && info) {
        if (api5.PrepareVkDevice(physical, info, &patched, &setup) == ONR_RR_OK && setup) {
            prepared = true;
            log("[openrr] Vulkan: PrepareVkDevice added runtime extensions/features/queue (%u extensions, %u queue families)",
                patched.enabledExtensionCount, patched.queueCreateInfoCount);
        } else {
            char err[256]{};
            if (api5.v4.v3.v2.v1.GetLastError) api5.v4.v3.v2.v1.GetLastError(err, sizeof err);
            log("[openrr] Vulkan: PrepareVkDevice left device as game asked (%s)", err);
        }
    }

    VkResult r = create(physical, prepared ? &patched : info, allocator, out);
    uint32_t qfam = 0, qidx = 0;
    if (prepared) {
        if (r != VK_SUCCESS) {
            api5.FinishVkDevice(setup, nullptr, nullptr, nullptr);
            setup = nullptr;
            prepared = false;
            log("[openrr] Vulkan: vkCreateDevice with runtime features failed (%d); retrying as the game asked", int(r));
            r = create(physical, info, allocator, out);
        } else {
            const int32_t frc = api5.FinishVkDevice(setup, out ? *out : nullptr, &qfam, &qidx);
            setup = nullptr;
            if (frc != ONR_RR_OK) prepared = false;
            else log("[openrr] Vulkan: FinishVkDevice succeeded (device %p, queue family %u index %u)",
                     out ? static_cast<void*>(*out) : nullptr, qfam, qidx);
        }
    }

    if (r == VK_SUCCESS && out && *out) {
        DeviceInfo di{};
        di.instance = instance;
        di.api_version = api_version ? api_version : VK_API_VERSION_1_3;
        di.physical = physical;
        di.device = *out;
        di.queue_family = qfam;
        di.queue_index = qidx;
        di.prepared = prepared;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_devices.erase(std::remove_if(g_devices.begin(), g_devices.end(),
                                           [d = *out](const DeviceInfo& x) { return x.device == d; }),
                            g_devices.end());
            g_devices.push_back(di);
        }
        if (out_info) *out_info = di;
    }
    return r;
}

bool get_device_info(VkDevice device, DeviceInfo* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_devices.rbegin(); it != g_devices.rend(); ++it) {
        if (!device || it->device == device) {
            if (out) *out = *it;
            return true;
        }
    }
    return false;
}

bool latest_device_info(DeviceInfo* out) {
    return get_device_info(VK_NULL_HANDLE, out);
}

void forget_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices.erase(std::remove_if(g_devices.begin(), g_devices.end(),
                                   [device](const DeviceInfo& x) { return x.device == device; }),
                    g_devices.end());
}

bool lock_queue_held(VkQueue queue) {
    if (!queue) return false;
    std::mutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& ptr = g_queue_locks[queue];
        if (!ptr) ptr = std::make_unique<std::mutex>();
        m = ptr.get();
    }
    m->lock();
    return true;
}

void unlock_queue_held(VkQueue queue, bool held) {
    if (!queue || !held) return;
    std::mutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_queue_locks.find(queue);
        if (it != g_queue_locks.end()) m = it->second.get();
    }
    if (m) m->unlock();
}

void set_adapter_names(const std::string& gpu, const std::string& driver) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!gpu.empty()) g_gpu_name = gpu;
    if (!driver.empty()) g_driver_name = driver;
}

std::string gpu_name() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gpu_name;
}

std::string driver_name() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_driver_name;
}

} // namespace openrr::pe::vkdevice
