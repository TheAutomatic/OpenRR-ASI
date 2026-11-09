// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_pe_config.hpp"
#include "rr_path_win.hpp"
#include "rr_number.hpp"
#include "rr_pe_log.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace openrr::pe {
namespace {

const bool number_reporting = [] {
    openrr::number_error = [](const std::string& value) {
        static std::mutex mutex;
        static std::set<std::string> said;
        std::lock_guard<std::mutex> guard(mutex);
        if (said.insert(value).second) log("[openrr] invalid number '%s': keeping the key's default", value.c_str());
    };
    return true;
}();

std::string trim(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<bool> parse_bool(const std::string& v) {
    const std::string s = lower(trim(v));
    if (s == "1" || s == "true" || s == "on" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "off" || s == "no") return false;
    return std::nullopt;
}

float clamp_float(const std::string& v, float lo, float hi, float def) {
    const float x = openrr::parse_float(v, def);
    return std::isnan(x) ? def : std::clamp(x, lo, hi);
}

uint64_t stamp_of(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!openrr::u8::attributes_ex(path.c_str(), &data)) return 0;
    return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// Key name table (from mochi nr_pe_config.cpp:91-120) extended with numeric virtual-key code parsing.
bool parse_hotkey(const std::string& text, int& vk, int& mods) {
    vk = 0;
    mods = 0;
    std::string rest = lower(trim(text));
    if (rest.empty() || rest == "none" || rest == "off") return false;

    while (!rest.empty()) {
        const auto plus = rest.find('+');
        const std::string part = trim(rest.substr(0, plus));
        rest = plus == std::string::npos ? std::string() : rest.substr(plus + 1);
        if (part.empty()) return false;
        if (part == "ctrl" || part == "control") mods |= MOD_CONTROL;
        else if (part == "shift") mods |= MOD_SHIFT;
        else if (part == "alt") mods |= MOD_ALT;
        else if (vk) return false;
        else if (part.size() >= 2 && part[0] == 'f' && std::isdigit(static_cast<unsigned char>(part[1]))) {
            const int n = openrr::parse_integer<int>(part.substr(1), -1);
            if (n < 1 || n > 24) return false;
            vk = VK_F1 + n - 1;
        } else if (part == "insert" || part == "ins") vk = VK_INSERT;
        else if (part == "delete" || part == "del") vk = VK_DELETE;
        else if (part == "home") vk = VK_HOME;
        else if (part == "end") vk = VK_END;
        else if (part == "pageup" || part == "pgup" || part == "prior") vk = VK_PRIOR;
        else if (part == "pagedown" || part == "pgdn" || part == "next") vk = VK_NEXT;
        else if (part == "pause") vk = VK_PAUSE;
        else if (part == "scrolllock" || part == "scroll") vk = VK_SCROLL;
        else if (part == "backspace" || part == "back") vk = VK_BACK;
        else if (part == "tab") vk = VK_TAB;
        else if (part == "enter" || part == "return") vk = VK_RETURN;
        else if (part == "space") vk = VK_SPACE;
        else if (part == "tilde" || part == "grave" || part == "`" || part == "~") vk = VK_OEM_3;
        else if (part == "backslash" || part == "\\") vk = VK_OEM_5;
        else if (part == "minus" || part == "-") vk = VK_OEM_MINUS;
        else if (part == "plus" || part == "equals" || part == "=") vk = VK_OEM_PLUS;
        else if (part.size() == 1 && std::isalpha(static_cast<unsigned char>(part[0]))) {
            vk = std::toupper(static_cast<unsigned char>(part[0]));
        } else if (part.size() == 1 && std::isdigit(static_cast<unsigned char>(part[0]))) {
            vk = int(static_cast<unsigned char>(part[0]));
        } else if (part.size() >= 2 && (part.rfind("0x", 0) == 0 || std::isdigit(static_cast<unsigned char>(part[0])))) {
            // Numeric virtual-key code (e.g. "45" or "0x2D" as used in OptiScaler.ini)
            char* end = nullptr;
            const long v = std::strtol(part.c_str(), &end, 0);
            if (end && *end == '\0' && v > 0 && v < 256) {
                vk = int(v);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return vk > 0 && vk < 256;
}

std::atomic<uint64_t> g_hotkeys_paused_until{0};
std::mutex g_cfg_mutex;
uint64_t g_cfg_stamp = 0;
ULONGLONG g_cfg_checked_ms = 0;
bool g_cfg_dirty = true;
Config g_cfg{};

const char kDefaultIni[] =
    "[OpenRR]\n"
    "; Enable or disable OpenRR (DLSS Ray Reconstruction on AMD RX 9000 / RDNA4 GPUs).\n"
    "Enabled = true\n\n"
    "; Execution backend for D3D12 games:\n"
    ";   auto   = D3D12 Direct ELF into the game's command list (falls back to Vulkan interop bridge if unavailable)\n"
    ";   d3d12  = D3D12 Direct ELF\n"
    ";   vulkan = D3D12 <-> Vulkan shared-resource interop bridge (splits command list around Vulkan compute)\n"
    "; Native Vulkan games always run directly on the game's Vulkan device (RecordVk).\n"
    "Backend = auto\n\n"
    "; Command-list split policy when coexisting with dlssnr-amd.asi:\n"
    ";   auto = disable command-list split and use D3D12 Direct when dlssnr-amd*.asi or an external vtable hook is detected\n"
    ";   off  = never install command-list split hooks\n"
    "CoexistSplit = auto\n\n"
    "; Menu toggle key:\n"
    ";   optiscaler = follow OptiScaler.ini [Menu] ShortcutKey (Insert by default, opens/closes with OptiScaler's menu)\n"
    ";   or any key name / virtual-key code (e.g. F7, Home, Ctrl+F7).\n"
    "MenuKey = optiscaler\n\n"
    "; Pass OptiScaler's ShortcutKey (Insert) and dlssnr-amd's MenuKey (End) through while the OpenRR menu is open.\n"
    "PassOptiScalerKey = 1\n\n"
    "; Network execution mode:\n"
    ";   new    = Fast mode (_lt kernels, recommended)\n"
    ";   legacy = Full-precision fast kernels\n"
    "Mode = new\n\n"
    "; Vulkan host input layout (shader_read or general)\n"
    "VkInputLayout = shader_read\n\n"
    "; Optional override for depth buffer type: auto, true (hardware depth), or false (linear depth)\n"
    "HardwareDepth = auto\n\n"
    "; Guide switches\n"
    "UseDisocclusionMask = true\n"
    "InvertDisocclusionMask = false\n"
    "ClampDisocclusionMask = true\n"
    "DisocclusionMaskScale = 1.0\n"
    "UseSssGuide = true\n"
    "UseResponsivityMask = false\n"
    "UseColorBeforeTransparency = false\n"
    "UseDepthOfFieldGuide = false\n"
    "UseReflectionMotionVectors = false\n"
    "UseLayerOpacityAndMotion = false\n";

Config load_file(const std::string& path) {
    Config c{};
    FILE* f = openrr::u8::fopen(path.c_str(), "rb");
    if (!f) return c;
    std::string raw;
    char buf[4096];
    for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) raw.append(buf, got);
    std::fclose(f);
    openrr::u8::decode_ini_text(raw);
    std::string section;
    for (size_t at = 0; at < raw.size();) {
        const size_t eol = raw.find('\n', at);
        std::string t = raw.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? raw.size() : eol + 1;
        const auto comment = t.find_first_of(";#");
        if (comment != std::string::npos) t = t.substr(0, comment);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') {
            const auto e = t.find(']');
            section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(t.substr(0, eq)));
        const std::string val = trim(t.substr(eq + 1));
        const std::string lval = lower(val);

        if (section == "openrr") {
            if (key == "enabled") { if (auto b = parse_bool(val)) c.enabled = *b; }
            else if (key == "backend") {
                if (lval == "auto" || lval == "d3d12" || lval == "vulkan") c.backend = lval;
            }
            else if (key == "coexistsplit") {
                if (lval == "auto" || lval == "off" || lval == "0" || lval == "false")
                    c.coexist_split = (lval == "auto") ? "auto" : "off";
            }
            else if (key == "menukey") c.menu_key = val;
            else if (key == "passoptiscalerkey") { if (auto b = parse_bool(val)) c.pass_optiscaler_key = *b; }
            else if (key == "mode") {
                if (lval == "new" || lval == "fast" || lval == "1") c.mode = "new";
                else if (lval == "legacy" || lval == "0") c.mode = "legacy";
            }
            else if (key == "vkinputlayout") {
                if (lval == "general" || lval == "shader_read" || lval == "read")
                    c.vk_input_layout = (lval == "general") ? "general" : "shader_read";
            }
            else if (key == "hardwaredepth") {
                if (lval == "auto" || lval.empty()) c.hardware_depth = std::nullopt;
                else if (auto b = parse_bool(val)) c.hardware_depth = *b;
            }
            else if (key == "usedisocclusionmask") { if (auto b = parse_bool(val)) c.use_disocclusion_mask = *b; }
            else if (key == "invertdisocclusionmask") { if (auto b = parse_bool(val)) c.invert_disocclusion_mask = *b; }
            else if (key == "clampdisocclusionmask") { if (auto b = parse_bool(val)) c.clamp_disocclusion_mask = *b; }
            else if (key == "disocclusionmaskscale") c.disocclusion_mask_scale = clamp_float(val, 0.01f, 10.0f, 1.0f);
            else if (key == "usesssguide") { if (auto b = parse_bool(val)) c.use_sss_guide = *b; }
            else if (key == "useresponsivitymask") { if (auto b = parse_bool(val)) c.use_responsivity_mask = *b; }
            else if (key == "usecolorbeforetransparency") { if (auto b = parse_bool(val)) c.use_color_before_transparency = *b; }
            else if (key == "usedepthoffieldguide") { if (auto b = parse_bool(val)) c.use_depth_of_field_guide = *b; }
            else if (key == "usereflectionmotionvectors") { if (auto b = parse_bool(val)) c.use_reflection_motion_vectors = *b; }
            else if (key == "uselayeropacityandmotion") { if (auto b = parse_bool(val)) c.use_layer_opacity_and_motion = *b; }
            else if (key == "menuscale") c.menu_scale = clamp_float(val, 0.5f, 3.0f, 1.0f);
            else if (key == "textscale") c.text_scale = clamp_float(val, 0.75f, 2.0f, 1.0f);
            else if (key == "theme") c.theme = std::clamp(openrr::parse_integer<int>(val, 0), 0, 5);
        }
    }
    return c;
}

} // namespace

Config load_config_file(const std::string& path) {
    return load_file(path);
}

bool parse_key(const std::string& text, int* vk, int* mods) {
    int k = 0, m = 0;
    const bool ok = parse_hotkey(text, k, m);
    if (vk) *vk = ok ? k : 0;
    if (mods) *mods = ok ? m : 0;
    return ok;
}

std::string vk_name(int vk, int mods) {
    if (vk <= 0 || vk >= 256) return "None";
    std::string prefix;
    if (mods & MOD_CONTROL) prefix += "Ctrl+";
    if (mods & MOD_SHIFT) prefix += "Shift+";
    if (mods & MOD_ALT) prefix += "Alt+";
    if (vk >= VK_F1 && vk <= VK_F24) return prefix + "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return prefix + std::string(1, char(vk));
    switch (vk) {
        case VK_INSERT: return prefix + "Insert";
        case VK_DELETE: return prefix + "Delete";
        case VK_HOME: return prefix + "Home";
        case VK_END: return prefix + "End";
        case VK_PRIOR: return prefix + "PageUp";
        case VK_NEXT: return prefix + "PageDown";
        case VK_PAUSE: return prefix + "Pause";
        case VK_SCROLL: return prefix + "ScrollLock";
        case VK_BACK: return prefix + "Backspace";
        case VK_TAB: return prefix + "Tab";
        case VK_RETURN: return prefix + "Enter";
        case VK_SPACE: return prefix + "Space";
        case VK_OEM_3: return prefix + "~";
        case VK_OEM_5: return prefix + "\\";
        case VK_OEM_MINUS: return prefix + "-";
        case VK_OEM_PLUS: return prefix + "=";
        default: {
            char b[16];
            std::snprintf(b, sizeof b, "0x%02X", vk);
            return prefix + b;
        }
    }
}

int read_optiscaler_shortcut_key(const std::string& folder, int* mods) {
    if (mods) *mods = 0;
    char k[64] = {};
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "OptiScaler.ini";
    openrr::u8::profile("Menu", "ShortcutKey", "auto", k, sizeof k, path);
    const std::string t = trim(k);
    if (t.empty() || lower(t) == "auto") return VK_INSERT;
    int vk = 0, m = 0;
    if (parse_key(t, &vk, &m)) {
        if (mods) *mods = m;
        return vk;
    }
    return VK_INSERT;
}

float read_optiscaler_menu_scale(const std::string& folder, float display_h) {
    static std::mutex scale_m;
    static ULONGLONG last_check_ms = 0;
    static uint64_t last_stamp = ~0ull;
    static float last_display_h = -1.0f;
    static float cached_scale = 1.0f;

    std::lock_guard<std::mutex> lock(scale_m);
    const ULONGLONG now = GetTickCount64();
    if (display_h == last_display_h && last_check_ms && (now - last_check_ms) < 500) {
        return cached_scale;
    }
    last_check_ms = now;
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "OptiScaler.ini";
    const uint64_t st = stamp_of(path);
    if (display_h == last_display_h && st == last_stamp) {
        return cached_scale;
    }
    last_display_h = display_h;
    last_stamp = st;

    char s[64] = {};
    openrr::u8::profile("Menu", "Scale", "auto", s, sizeof s, path);
    const std::string t = lower(trim(s));
    if (!t.empty() && t != "auto") {
        const float v = openrr::parse_float(t, 0.0f);
        if (std::isfinite(v) && v >= 0.5f && v <= 4.0f) {
            cached_scale = v;
            return cached_scale;
        }
    }
    cached_scale = std::max(0.5f, std::floor(display_h / 108.0f) / 10.0f);
    return cached_scale;
}

int read_mochi_menu_key(const std::string& folder, int* mods) {
    if (mods) *mods = 0;
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "dlssnr-amd.ini";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!openrr::u8::attributes_ex(path.c_str(), &data)) return VK_END;
    char k[64] = {};
    openrr::u8::profile("NRControl", "MenuKey", "End", k, sizeof k, path);
    const std::string t = trim(k);
    if (t.empty()) return 0;
    int vk = 0, m = 0;
    if (parse_key(t, &vk, &m)) {
        if (mods) *mods = m;
        return vk;
    }
    return VK_END;
}

void restore_ini(const std::string& folder) {
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "openrr-amd.ini";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (openrr::u8::attributes_ex(path.c_str(), &data)) return;
    if (FILE* f = openrr::u8::fopen(path.c_str(), "wb")) {
        std::fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, f);
        std::fclose(f);
        log("[openrr] wrote default %s", path.c_str());
    }
}

void touch_config() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    g_cfg_dirty = true;
}

Config current_config() {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    const ULONGLONG now = GetTickCount64();
    if (!g_cfg_dirty && g_cfg_checked_ms && now - g_cfg_checked_ms < 500) {
        return g_cfg;
    }
    g_cfg_checked_ms = now;
    const std::string folder = module_folder();
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "openrr-amd.ini";
    const uint64_t st = stamp_of(path);
    if (g_cfg_dirty || st != g_cfg_stamp) {
        g_cfg = load_file(path);
        g_cfg_stamp = st;
        g_cfg_dirty = false;
    }
    return g_cfg;
}

void pause_hotkeys() {
    g_hotkeys_paused_until.store(GetTickCount64() + 500);
}

bool hotkeys_paused() {
    return GetTickCount64() < g_hotkeys_paused_until.load();
}

} // namespace openrr::pe
