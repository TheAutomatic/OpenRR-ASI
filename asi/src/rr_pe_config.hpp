// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace openrr::pe {

struct Config {
    bool enabled = true;
    std::string backend = "auto";                  // auto, d3d12, vulkan
    std::string coexist_split = "auto";            // auto, off
    int mode = 1;                                  // OnrRrSetFastMode: 0 legacy, 1 new, 2 experimental
    std::string vk_input_layout = "shader_read";   // shader_read, general
    std::optional<bool> hardware_depth = std::nullopt; // nullopt = auto, true = HW, false = linear

    bool use_disocclusion_mask = true;
    bool invert_disocclusion_mask = false;
    bool clamp_disocclusion_mask = true;
    float disocclusion_mask_scale = 1.0f;
    bool use_sss_guide = true;
    bool use_responsivity_mask = false;
    bool use_color_before_transparency = false;
    bool use_depth_of_field_guide = false;
    bool use_reflection_motion_vectors = false;
    bool use_layer_opacity_and_motion = false;
};

int network_mode_level(const std::string& value);

// Ensure openrr-amd.ini exists in folder (writes default template if missing).
void restore_ini(const std::string& folder);

// Read openrr-amd.ini (cached by file timestamp; force reload via touch_config()).
Config load_config_file(const std::string& path);
Config current_config();
void touch_config();

} // namespace openrr::pe
