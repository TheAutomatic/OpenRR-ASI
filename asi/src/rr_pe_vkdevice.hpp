// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace openrr::pe {
// Ensure vulkan-1.dll entry points are resolved (vulkan_lazy.c).
void vulkan_entry_points();
} // namespace openrr::pe

namespace openrr::pe::vkdevice {

struct DeviceInfo {
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t api_version = VK_API_VERSION_1_3;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t queue_index = 0;
    bool prepared = false;
};

// Called inside hooked vkCreateDevice: runs OnrRrApi5::PrepareVkDevice + create + OnrRrApi5::FinishVkDevice.
VkResult create_with_runtime(PFN_vkCreateDevice create, VkInstance instance, uint32_t api_version,
                             VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                             const VkAllocationCallbacks* allocator, VkDevice* out, DeviceInfo* out_info);

bool get_device_info(VkDevice device, DeviceInfo* out);
bool latest_device_info(DeviceInfo* out);
void forget_device(VkDevice device);

// Per-queue lock helpers for Vulkan menu overlay submission.
bool lock_queue_held(VkQueue queue);
void unlock_queue_held(VkQueue queue, bool held);

void set_adapter_names(const std::string& gpu, const std::string& driver);
std::string gpu_name();
std::string driver_name();

} // namespace openrr::pe::vkdevice
