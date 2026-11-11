// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include <vulkan/vulkan.h>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct ID3D12CommandList;
struct ID3D11Device;
struct IUnknown;

namespace openrr::pe::menu {

void install();
void prepare_dxgi(); // Synchronous factory-method hooks; outside loader lock.
// Non-test Present heartbeat, independent of menu visibility and queue availability.
using PresentBeatHook = void (*)();
void on_present_beat(PresentBeatHook hook);
void hook_swapchain_vtable(IDXGISwapChain* sc);

using D3D12PresentHook = void (*)(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, int encoding);
void on_d3d12_present(D3D12PresentHook hook);

using D3D12ShownHook = void (*)(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue);
void on_d3d12_shown(D3D12ShownHook hook);

using D3D11PresentHook = void (*)(IDXGISwapChain* swap_chain, ID3D11Device* device);
void on_d3d11_present(D3D11PresentHook hook);

void draw_game(IDXGISwapChain* swap_chain, IUnknown* device, bool new_frame = true);
bool colour_space_of(IDXGISwapChain* swap_chain, int* space);
void abandon_at_exit();

const VkPresentInfoKHR* vulkan_present(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                                       VkQueue queue, uint32_t family, VkSwapchainKHR sc, VkFormat format,
                                       VkColorSpaceKHR space, VkExtent2D extent, const std::vector<VkImage>& images,
                                       const VkPresentInfoKHR* info);
void vulkan_forget(VkDevice device, VkSwapchainKHR sc = VK_NULL_HANDLE);

} // namespace openrr::pe::menu
