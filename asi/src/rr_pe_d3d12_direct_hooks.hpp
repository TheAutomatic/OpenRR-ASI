#pragma once

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace openrr::pe {

struct HookOps {
    MH_STATUS(WINAPI* create_hook)(LPVOID target, LPVOID detour, LPVOID* orig) = &MH_CreateHook;
    MH_STATUS(WINAPI* remove_hook)(LPVOID target) = &MH_RemoveHook;
    MH_STATUS(WINAPI* enable_hook)(LPVOID target) = &MH_EnableHook;
    MH_STATUS(WINAPI* disable_hook)(LPVOID target) = &MH_DisableHook;
    MH_STATUS(WINAPI* queue_enable)(LPVOID target) = &MH_QueueEnableHook;
    MH_STATUS(WINAPI* queue_disable)(LPVOID target) = &MH_QueueDisableHook;
    MH_STATUS(WINAPI* apply_queued)() = &MH_ApplyQueued;
};

struct BatchHookItem {
    const char* name = nullptr;
    unsigned slot = 0;
    void* target = nullptr;
    void* detour = nullptr;
    void** orig = nullptr;
};

enum class BatchInstallError {
    Ok = 0,
    CreateFailed,
    QueueFailed,
    ApplyFailed,
};

struct BatchInstallResult {
    BatchInstallError error = BatchInstallError::Ok;
    size_t failed_index = 0;
    MH_STATUS mh_status = MH_OK;
    // True when MH_ApplyQueued() failed after possibly enabling a prefix of hooks: trampolines and
    // *orig pointers were intentionally kept alive so in-flight threads never crash.
    bool trampolines_retained = false;
};

// Enables a set of ALREADY-CREATED hooks in one MinHook freeze (MH_QueueEnableHook + MH_ApplyQueued).
// - If queueing fails before MH_ApplyQueued(): safely unqueues, removes all items, and clears *orig.
// - If MH_ApplyQueued() fails: a prefix of hooks may already be live and entered by other threads;
//   disables all items to block new entries and clears queueEnable, while intentionally retaining
//   trampolines and *orig pointers so in-flight threads can finish calling the original function.
inline BatchInstallResult enable_created_hook_batch(const BatchHookItem* items, size_t count,
                                                    const HookOps& ops = {}) {
    BatchInstallResult res{};
    if (!items || count == 0) return res;

    for (size_t i = 0; i < count; ++i) {
        const MH_STATUS st = ops.queue_enable(items[i].target);
        if (st != MH_OK) {
            for (size_t j = 0; j < i; ++j) ops.queue_disable(items[j].target);
            for (size_t j = 0; j < count; ++j) {
                ops.remove_hook(items[j].target);
                if (items[j].orig) *items[j].orig = nullptr;
            }
            res.error = BatchInstallError::QueueFailed;
            res.failed_index = i;
            res.mh_status = st;
            return res;
        }
    }

    const MH_STATUS apply_st = ops.apply_queued();
    if (apply_st == MH_OK) return res;

    // MH_ApplyQueued() breaks on first EnableHookLL failure and unfreezes threads, leaving earlier
    // hooks enabled and later hooks still marked queueEnable=TRUE. Queue-disable every target in the
    // batch to clear queueEnable and disable any enabled prefix, without removing trampolines or
    // clearing *orig.
    for (size_t i = 0; i < count; ++i) ops.queue_disable(items[i].target);
    if (ops.apply_queued() != MH_OK) {
        for (size_t i = 0; i < count; ++i) ops.disable_hook(items[i].target);
    }

    res.error = BatchInstallError::ApplyFailed;
    res.mh_status = apply_st;
    res.trampolines_retained = true;
    return res;
}

// Atomically creates (all-or-nothing) and batch-enables a set of hooks.
inline BatchInstallResult install_hook_batch(const BatchHookItem* items, size_t count,
                                             const HookOps& ops = {}) {
    BatchInstallResult res{};
    if (!items || count == 0) return res;

    for (size_t i = 0; i < count; ++i) {
        const MH_STATUS st = ops.create_hook(items[i].target, items[i].detour, items[i].orig);
        if (st != MH_OK) {
            for (size_t j = 0; j < i; ++j) {
                ops.remove_hook(items[j].target);
                if (items[j].orig) *items[j].orig = nullptr;
            }
            if (items[i].orig) *items[i].orig = nullptr;
            res.error = BatchInstallError::CreateFailed;
            res.failed_index = i;
            res.mh_status = st;
            return res;
        }
    }
    return enable_created_hook_batch(items, count, ops);
}

enum class EntryHookOutcome {
    AlreadyCovered = 0,
    Installed,
    AlreadyFailed,
    RefusedPolicy,
    DuplicateOtherHook,
    SlotsFull,
    CreateFailed,
    EnableFailed,
};

// Multi-slot registry for hooking a single D3D12 method across multiple distinct device/object
// implementations (e.g. native D3D12Core + a wrapper) without a one-shot global latch.
template <size_t MaxSlots>
struct EntryHookRegistry {
    struct Slot {
        std::atomic<void*> target{nullptr};
        void* orig = nullptr;
    };

    Slot slots[MaxSlots]{};
    std::atomic<size_t> count{0};
    std::vector<void*> failed_targets;

    bool is_covered_lockfree(const void* fn) const {
        if (!fn) return false;
        const size_t n = count.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (slots[i].target.load(std::memory_order_relaxed) == fn) return true;
        }
        return false;
    }

    bool has_failed_locked(const void* fn) const {
        return fn && std::find(failed_targets.begin(), failed_targets.end(), fn) != failed_targets.end();
    }

    void note_failed_locked(void* fn) {
        if (fn && !has_failed_locked(fn)) failed_targets.push_back(fn);
    }
};

// Caller must hold the hook-install mutex.
// `detours` must point to an array of `MaxSlots` per-slot detour entry points matching `reg.slots[i].orig`.
// `is_hookable(fn, &mod_name)` validates policy; `known_hooked` prevents hooking the same function under
// two unrelated hook families. Failures are recorded per-entry (`failed_targets`) so a different entry
// from another device can still be retried and hooked.
template <size_t MaxSlots, class HookablePred>
inline EntryHookOutcome ensure_entry_hook_locked(EntryHookRegistry<MaxSlots>& reg, void* fn,
                                                 void* const (&detours)[MaxSlots],
                                                 std::vector<void*>& known_hooked,
                                                 HookablePred&& is_hookable,
                                                 std::string* out_mod = nullptr,
                                                 MH_STATUS* out_mh = nullptr,
                                                 const HookOps& ops = {}) {
    if (out_mh) *out_mh = MH_OK;
    if (reg.is_covered_lockfree(fn)) return EntryHookOutcome::AlreadyCovered;
    if (reg.has_failed_locked(fn)) return EntryHookOutcome::AlreadyFailed;

    std::string mod;
    if (!is_hookable(fn, &mod)) {
        if (out_mod) *out_mod = mod;
        reg.note_failed_locked(fn);
        return EntryHookOutcome::RefusedPolicy;
    }
    if (out_mod) *out_mod = mod;

    if (std::find(known_hooked.begin(), known_hooked.end(), fn) != known_hooked.end()) {
        reg.note_failed_locked(fn);
        return EntryHookOutcome::DuplicateOtherHook;
    }

    const size_t idx = reg.count.load(std::memory_order_relaxed);
    if (idx >= MaxSlots) {
        reg.note_failed_locked(fn);
        return EntryHookOutcome::SlotsFull;
    }

    void* orig = nullptr;
    const MH_STATUS cr = ops.create_hook(fn, detours[idx], &orig);
    if (cr != MH_OK) {
        if (out_mh) *out_mh = cr;
        reg.note_failed_locked(fn);
        return EntryHookOutcome::CreateFailed;
    }

    reg.slots[idx].orig = orig;
    // Single-hook MH_EnableHook only fails before patching target bytes (VirtualProtect failure),
    // so removing the unenabled trampoline on failure is safe.
    const MH_STATUS en = ops.enable_hook(fn);
    if (en != MH_OK) {
        if (out_mh) *out_mh = en;
        ops.remove_hook(fn);
        reg.slots[idx].orig = nullptr;
        reg.note_failed_locked(fn);
        return EntryHookOutcome::EnableFailed;
    }

    reg.slots[idx].target.store(fn, std::memory_order_relaxed);
    reg.count.store(idx + 1, std::memory_order_release);
    known_hooked.push_back(fn);
    return EntryHookOutcome::Installed;
}

}  // namespace openrr::pe
