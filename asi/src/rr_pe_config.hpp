// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace openrr::pe {

struct Config {
    bool enabled = true;
    std::string backend = "auto";          // auto, d3d12, vulkan
    std::string coexist_split = "auto";    // auto, off
    std::string menu_key = "optiscaler";   // optiscaler, or key name / VK code
    bool pass_optiscaler_key = true;
    std::string mode = "new";              // new (fast _lt), legacy (full-precision _f)
    std::string vk_input_layout = "shader_read"; // shader_read, general
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

    float menu_scale = 1.0f;
    float text_scale = 1.0f;
    int theme = 0;
};

// Parse a key name ("Insert", "End", "F7", "Ctrl+Shift+F10") or virtual-key number ("45", "0x2D") into vk and MOD_* bits.
bool parse_key(const std::string& text, int* vk, int* mods = nullptr);
std::string vk_name(int vk, int mods = 0);

// Resolve OptiScaler's [Menu] ShortcutKey from OptiScaler.ini (defaults to VK_INSERT = 0x2D).
int read_optiscaler_shortcut_key(const std::string& folder, int* mods = nullptr);

// Resolve OptiScaler's [Menu] Scale from OptiScaler.ini (auto -> max(0.5, floor(display_h / 108) / 10)).
float read_optiscaler_menu_scale(const std::string& folder, float display_h);

// Resolve mochi's [NRControl] MenuKey from dlssnr-amd.ini (defaults to VK_END = 0x23).
int read_mochi_menu_key(const std::string& folder, int* mods = nullptr);

// Ensure openrr-amd.ini exists in folder (writes default template if missing).
void restore_ini(const std::string& folder);

// Read openrr-amd.ini (cached by file timestamp; force reload via touch_config()).
Config load_config_file(const std::string& path);
Config current_config();
void touch_config();

// Pause hotkeys briefly while the menu captures a key binding.
void pause_hotkeys();
bool hotkeys_paused();

} // namespace openrr::pe
