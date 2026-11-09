// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include "rr_control_shared.hpp"
#include <cstdint>
#include <string>

namespace openrr::pe::control {

struct Status {
    uint32_t route = 0;            // 1 D3D12, 2 Vulkan
    bool rr_active = false;
    uint32_t state = 0;
    uint32_t backend = 0;
    uint32_t fast_mode = 0;
    uint32_t frames = 0;
    float gpu_ms_last = 0.0f;
    float gpu_ms_average = 0.0f;
    float gpu_raw_last = 0.0f;
    float gpu_raw_p50 = 0.0f;
    float gpu_raw_p95 = 0.0f;
    uint32_t render_width = 0, render_height = 0;
    uint32_t output_width = 0, output_height = 0;
    bool coexist_mochi = false;
    bool split_active = false;
    std::string weights_source;
    std::string message;
    std::string backend_message;
};

void report(const Status& s);
void start_input();
openrr::control::Shared* page();
void asi_facts(const char* build);

} // namespace openrr::pe::control
