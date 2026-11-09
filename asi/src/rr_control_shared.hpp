// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include <cstdint>

namespace openrr::control {

constexpr uint32_t kMagic = 0x52524354u; // "RRCT"
constexpr uint32_t kVersion = 1;

struct Shared {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;

    // Input arbitration state (written by watch() / window_proc, read by menu)
    uint32_t wayland = 1;
    uint32_t menu_open = 0;
    uint32_t game_focused = 0;
    uint32_t cursor_shown = 0;
    int32_t mouse_x = 0, mouse_y = 0;
    uint32_t client_w = 0, client_h = 0;
    uint32_t buttons = 0;
    int32_t wheel = 0;
    uint32_t keys[8]{};
    uint32_t chars[64]{};
    uint32_t chars_written = 0;
    uint32_t input_seq = 0;
    uint32_t menu_seq = 0;
    uint32_t dll_menu = 0;

    // OpenRR status (written by evaluate / runtime, read by menu)
    uint32_t status_seq = 0;
    uint32_t route = 0;            // 0 idle, 1 D3D12, 2 Vulkan
    uint32_t rr_active = 0;        // 1 if RR feature is created and active
    uint32_t state = 0;            // OnrRrState (0 building, 1 ready, 2 failed)
    uint32_t backend = 0;          // OnrRrBackend (0 Vulkan bridge, 1 D3D12 direct, 2 Vulkan host)
    uint32_t fast_mode = 0;        // OnrRrGetFastMode (0 off, 1 on, 2 building, 3 n/a)
    uint32_t frames = 0;
    float gpu_ms_last = 0.0f;
    float gpu_ms_average = 0.0f;
    float gpu_raw_last = 0.0f;
    float gpu_raw_p50 = 0.0f;
    float gpu_raw_p95 = 0.0f;
    uint32_t render_width = 0, render_height = 0;
    uint32_t output_width = 0, output_height = 0;
    uint32_t coexist_mochi = 0;    // 1 if dlssnr-amd*.asi is detected in the process
    uint32_t split_active = 0;     // 1 if command-list split is active for this session
    char asi_build[64]{};
    char weights_source[260]{};
    char message[256]{};
    char backend_message[128]{};
    char gpu_name[128]{};
    char driver_name[128]{};
};

} // namespace openrr::control
