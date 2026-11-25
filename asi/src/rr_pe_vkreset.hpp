// Shared Vulkan command-buffer / command-pool tracking wrappers so ASI and unit tests
// exercise the exact same driver-first, success-only retirement notification logic.
#pragma once

#include <vulkan/vulkan.h>

namespace openrr::pe::vkreset {

using PfnNotifyAllocCbs = void (*)(void*, void*, uint32_t, void* const*);
using PfnNotifyCbReset = void (*)(void*);
using PfnNotifyCbsFreed = void (*)(void*, void*, uint32_t, void* const*);
using PfnNotifyPoolReset = void (*)(void*, void*);
using PfnNotifyPoolDestroyed = void (*)(void*, void*);

inline VkResult track_alloc_cbs(PFN_vkAllocateCommandBuffers next, PfnNotifyAllocCbs notify,
                                VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    const VkResult r = next(dev, ai, out);
    if (r == VK_SUCCESS && ai && out && notify)
        notify(dev, ai->commandPool, ai->commandBufferCount, reinterpret_cast<void* const*>(out));
    return r;
}

inline VkResult track_begin_cb(PFN_vkBeginCommandBuffer next, PfnNotifyCbReset notify,
                               VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    const VkResult r = next(cb, bi);
    if (r == VK_SUCCESS && cb && notify) notify(cb);
    return r;
}

inline VkResult track_reset_cb(PFN_vkResetCommandBuffer next, PfnNotifyCbReset notify,
                               VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    const VkResult r = next(cb, flags);
    if (r == VK_SUCCESS && cb && notify) notify(cb);
    return r;
}

inline void track_free_cbs(PFN_vkFreeCommandBuffers next, PfnNotifyCbsFreed notify,
                           VkDevice dev, VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs) {
    if (cbs && count && notify)
        notify(dev, pool, count, reinterpret_cast<void* const*>(const_cast<VkCommandBuffer*>(cbs)));
    next(dev, pool, count, cbs);
}

inline VkResult track_reset_pool(PFN_vkResetCommandPool next, PfnNotifyPoolReset notify,
                                 VkDevice dev, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    const VkResult r = next(dev, pool, flags);
    if (r == VK_SUCCESS && pool && notify) notify(dev, pool);
    return r;
}

inline void track_destroy_pool(PFN_vkDestroyCommandPool next, PfnNotifyPoolDestroyed notify,
                               VkDevice dev, VkCommandPool pool, const VkAllocationCallbacks* a) {
    if (pool && notify) notify(dev, pool);
    next(dev, pool, a);
}

} // namespace openrr::pe::vkreset
