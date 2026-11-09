// SPDX-License-Identifier: MIT
// FLOW (the _fl kernels, enc2 .. dec2_1): consecutive kernels of the small levels run without a barrier between them.
// Each window of kernel FLOW_SLOT counts itself done in its window row; a window of the next kernel (FLOW_WAIT) first
// waits for the rows of the previous kernel its inputs come from (one row of margin each side, the whole kernel when its
// band touches the last row). Correct because the windows of a dispatch are all launched before those of the next one
// (same queue, in order): a waiting window never holds back the windows it waits for. The flags come in pc.mask (unused
// by the fast bodies), zeroed by the engine every frame: per slot FLOW_WORDS words, [0, FLOW_ROWS) windows done per row,
// [FLOW_ROWS, 2 FLOW_ROWS) row done, then windows done and all done. The engine keeps the barriers when a level has more
// window rows than FLOW_ROWS.
#extension GL_KHR_memory_scope_semantics : require
#define FLOW_ROWS 240u
#define FLOW_ROW_REPS 4u
#define FLOW_ALL_BASE (5u * FLOW_ROWS)
#define FLOW_WORDS 1280u
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FlowFlags { uint v[]; };
FlowFlags flow_flags() { return FlowFlags(uint64_t(pc.mask)); }
#if FLOW_WAIT
#ifndef FLOW_BACKOFF
#define FLOW_BACKOFF 32u
#endif
// producer pixel rows [ya, yb] at producer height hp
void flow_wait_rows(int ya, int yb, int hp) {
    if (gl_LocalInvocationIndex == 0u) {
        FlowFlags f = flow_flags();
        uint b = uint(FLOW_SLOT - 1) * FLOW_WORDS;
        uint wg_hash = gl_WorkGroupID.x + gl_WorkGroupID.y * 5u + gl_WorkGroupID.z * 11u;
        uint spin = wg_hash | 1u;
        int lo = max(ya / 8 - 1, 0), hi = yb / 8 + 1;
        if (hi >= (hp - 1) / 8) {
            uint rep = wg_hash & 15u;
            for (uint w = 1u; w != 0u; ++w) {
                if (atomicLoad(f.v[b + FLOW_ALL_BASE + 1u + rep], gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire) != 0u)
                    break;
                for (uint k = 0u; k < FLOW_BACKOFF; ++k) spin = spin * 1664525u + 1013904223u;
                if (spin == 0x9e3779b9u) w = 1u;
            }
        } else {
            uint rrep = wg_hash & (FLOW_ROW_REPS - 1u);
            for (int r = lo; r <= hi; ++r) {
                uint idx = b + FLOW_ROWS + FLOW_ROW_REPS * uint(r) + rrep;
                for (uint w = 1u; w != 0u; ++w) {
                    if (atomicLoad(f.v[idx], gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire) != 0u)
                        break;
                    for (uint k = 0u; k < FLOW_BACKOFF; ++k) spin = spin * 1664525u + 1013904223u;
                    if (spin == 0x9e3779b9u) w = 1u;
                }
            }
        }
    }
    barrier();
    memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire);
}
void flow_wait(ivec2 win) {
    int y0 = 8 * win.y - pc.shift.y, y1 = y0 + 7;
    y0 = max(y0, 0), y1 = min(max(y1, 7), pc.size.y - 1);
#if ENC_STAGE
    flow_wait_rows(2 * y0, 2 * y1 + 1, 2 * pc.size.y);   // the previous level's pooled output, at 2x
#elif DEC_UP
    flow_wait_rows(max(y0 / 2 - 1, 0), y1 / 2 + 1, pc.lo_size.y);   // the level below, at 1/2
#else
    flow_wait_rows(y0, y1, pc.size.y);   // dec_dfn: the dec_up of the level
#endif
}
#else
void flow_wait(ivec2 win) {}
#endif
void flow_signal(ivec2 win) {
    memoryBarrierBuffer();
    barrier();
    if (gl_LocalInvocationIndex == 0u) {
        FlowFlags f = flow_flags();
        uint b = uint(FLOW_SLOT) * FLOW_WORDS;
        uint rows = uint((pc.size.y + pc.shift.y + 7) / 8);
        uint n = atomicAdd(f.v[b + uint(win.y)], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquireRelease);
        if (n + 1u == uint(pc.nwx)) {
            uint rbase = b + FLOW_ROWS + FLOW_ROW_REPS * uint(win.y);
            for (uint rep = 0u; rep < FLOW_ROW_REPS; ++rep)
                atomicStore(f.v[rbase + rep], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
        }
        uint t = atomicAdd(f.v[b + FLOW_ALL_BASE], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquireRelease);
        if (t + 1u == uint(pc.nwx) * rows) {
            for (uint rep = 0u; rep < 16u; ++rep)
                atomicStore(f.v[b + FLOW_ALL_BASE + 1u + rep], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
        }
    }
}
