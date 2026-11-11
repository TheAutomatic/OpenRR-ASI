// SPDX-License-Identifier: MIT
#include "rr_present_scope.hpp"
#include <cstdio>
#include <thread>
#include <stdexcept>
using openrr::pe::menu::PresentScope;
struct Counts { int draws = 0, beats = 0, native = 0; };
static int native_present(Counts& c) {
    const PresentScope scope;
    if (scope.outermost()) { ++c.draws; ++c.beats; }
    ++c.native;
    return 17;
}
static int wrapped_present(Counts& c) {
    const PresentScope scope;
    if (scope.outermost()) { ++c.draws; ++c.beats; }
    return native_present(c);
}
static int wrapped_present1(Counts& c) {
    const PresentScope scope;
    if (scope.outermost()) { ++c.draws; ++c.beats; }
    return wrapped_present(c);
}
int main() {
    Counts c;
    if (wrapped_present(c) != 17 || wrapped_present1(c) != 17 ||
        c.draws != 2 || c.beats != 2 || c.native != 2) return 1;
    // Other threads must still draw while this thread is inside Present.
    Counts other;
    {
        const PresentScope scope;
        std::thread t([&] { wrapped_present1(other); });
        t.join();
        if (!scope.outermost() || other.draws != 1 || other.native != 1) return 2;
    }
    try { const PresentScope scope; throw std::runtime_error("test"); } catch (...) {}
    if (native_present(c) != 17 || c.draws != 3 || c.beats != 3 || c.native != 3) return 3;
    std::puts("PASS: nested Present/Present1 draw and beat once, forward native result, restore next frame/exception, isolate threads");
}
