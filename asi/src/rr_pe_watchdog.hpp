// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once
// A stall report in openrr-amd.log: when the game's frames stop for a while (or none came at all a minute after the
// module loaded), what every thread of the process is doing - each thread's stack as return addresses into the
// modules (module+offset, with the nearest export when the module has one) and, for the threads in OpenRR's own code, the
// step they are in (Phase). A game that freezes leaves the place it froze in the log instead of nothing.
//
// The report suspends the threads one at a time only long enough to copy their registers and stack (nothing is
// allocated or locked meanwhile) and writes after they run again. At most four reports a process; enabled via OPENRR_WATCHDOG=1.
// Implemented in rr_pe_log.cpp.
namespace openrr::pe::watchdog {

// Starts the stall-monitoring thread (called from deferred post-DllMain init).
void start();

// Enable stall monitoring only while at least one OpenRR feature context is active.
void set_rr_active(bool active);

// An active RR frame went by. Starts the watchdog if start() was not called.
void beat();

// The step this thread is in, while the object lives (nested steps restore the outer one). `what` must be a literal.
struct Phase {
    explicit Phase(const char* what);
    ~Phase();
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
    const char* previous;
};

}  // namespace openrr::pe::watchdog
