// SPDX-License-Identifier: MIT
#pragma once

namespace openrr::pe::menu {
// Shared by Present/Present1 and all hook slots, including wrapped/native chains.
class PresentScope {
    static inline thread_local bool active = false;
    const bool nested = active;
public:
    PresentScope() { active = true; }
    ~PresentScope() { active = nested; }
    bool outermost() const { return !nested; }
    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;
};
}
