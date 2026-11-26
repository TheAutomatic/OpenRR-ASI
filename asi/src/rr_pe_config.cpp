// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_pe_config.hpp"
#include "rr_path_win.hpp"
#include "rr_number.hpp"
#include "rr_pe_log.hpp"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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

std::mutex g_cfg_mutex;
uint64_t g_cfg_stamp = 0;
ULONGLONG g_cfg_checked_ms = 0;
bool g_cfg_dirty = true;
Config g_cfg{};

const char kDefaultIni[] =
    "[OpenRR]\n"
    "; Register OpenRR as an OptiScaler Ray Reconstruction (DLSSD) provider (restart required when changed).\n"
    "; To switch providers live in-game, use OptiScaler's Upscalers -> RR Upscaler selector.\n"
    "Enabled = true\n\n"
    "; Menu language inside OptiScaler's PLUGINS -> OpenRR tab:\n"
    ";   empty (English), or stem of openrr-amd\\lang\\<name>.txt (e.g. zh-CN, ja, ko)\n"
    "; Language =\n\n"
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
    "; Network execution mode:\n"
    ";   new    = Fast mode level 1 (_lt kernels, recommended)\n"
    ";   fast   = Fast mode level 2 (experimental _lx kernels; falls back to level 1 if absent)\n"
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
            else if (key == "mode") {
                c.mode = network_mode_level(lval);
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
        }
    }
    return c;
}

} // namespace

int network_mode_level(const std::string& value) {
    const auto mode = lower(trim(value));
    if (mode == "legacy" || mode == "0") return 0;
    if (mode == "fast" || mode == "experimental" || mode == "2") return 2;
    return 1;
}

Config load_config_file(const std::string& path) {
    return load_file(path);
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

} // namespace openrr::pe
