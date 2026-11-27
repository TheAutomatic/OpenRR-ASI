// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_pe_control.hpp"

#include <cstdio>
#include <mutex>

namespace openrr::pe::control {
namespace {

std::mutex g_mutex;
openrr::control::Shared g_state{};

} // namespace

void asi_facts(const char* build) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (build) std::snprintf(g_state.asi_build, sizeof(g_state.asi_build), "%s", build);
}

void report(const Status& st) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.route = st.route;
    g_state.rr_active = st.rr_active ? 1u : 0u;
    g_state.state = st.state;
    g_state.backend = st.backend;
    g_state.fast_mode = st.fast_mode;
    g_state.frames = st.frames;
    g_state.gpu_ms_last = st.gpu_ms_last;
    g_state.gpu_ms_average = st.gpu_ms_average;
    g_state.gpu_raw_last = st.gpu_raw_last;
    g_state.gpu_raw_p50 = st.gpu_raw_p50;
    g_state.gpu_raw_p95 = st.gpu_raw_p95;
    g_state.render_width = st.render_width;
    g_state.render_height = st.render_height;
    g_state.output_width = st.output_width;
    g_state.output_height = st.output_height;
    g_state.coexist_mochi = st.coexist_mochi ? 1u : 0u;
    std::snprintf(g_state.weights_source, sizeof(g_state.weights_source), "%s", st.weights_source.c_str());
    std::snprintf(g_state.message, sizeof(g_state.message), "%s", st.message.c_str());
    std::snprintf(g_state.backend_message, sizeof(g_state.backend_message), "%s", st.backend_message.c_str());
}

openrr::control::Shared snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

} // namespace openrr::pe::control
