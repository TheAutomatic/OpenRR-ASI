// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_path_win.hpp"
#include "rr_number.hpp"
#include "rr_pe_menu.hpp"
#include "mingw_d3d12_compat.h"
#include "rr_control_shared.hpp"
#include "rr_ini_edit.hpp"
#include "rr_menu_font.hpp"
#include "rr_pe_config.hpp"
#include "rr_pe_control.hpp"
#include "rr_pe_log.hpp"
#include "rr_pe_vkdevice.hpp"
#include "opennr_rr_api.h"

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_vulkan.h"
#include "menu_compose_vert.h"
#include "menu_compose_frag.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef OPENRR_BUILD_STAMP
#define OPENRR_BUILD_STAMP "dev"
#endif

extern "C" HRESULT WINAPI D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                UINT, ID3DBlob**, ID3DBlob**);
    static const Fn fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) return E_NOTIMPL;
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                                      ID3DBlob** blob, ID3DBlob** errors) {
    using Fn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    const Fn fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "D3D12SerializeRootSignature"))) : nullptr;
    return fn ? fn(desc, version, blob, errors) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE m = GetModuleHandleW(L"dxgi.dll");
    const Fn fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "CreateDXGIFactory1"))) : nullptr;
    return fn ? fn(riid, factory) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    HMODULE m = GetModuleHandleW(L"dxgi.dll");
    const Fn fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "CreateDXGIFactory2"))) : nullptr;
    return fn ? fn(flags, riid, factory) : E_NOTIMPL;
}

HMODULE dllModule = nullptr;

namespace openrr::pe::menu {
namespace {

void say(const std::string& s) { log("[openrr] OpenRR menu: %s", s.c_str()); }

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

openrr::control::Shared* shared() { return openrr::pe::control::page(); }
std::filesystem::path game_dir() { return std::filesystem::path(openrr::pe::module_folder()); }

using openrr::ini_edit::Ini;

uint64_t g_drawn = 0;

bool open_at_start() {
    const char* e = std::getenv("OPENRR_CONTROL_OPEN");
    return e && e[0] == '1';
}

struct Input {
    bool open = open_at_start();
    int open_vk = VK_INSERT, open_mods = 0;
    uint32_t keys_before[8]{};
    int32_t wheel_seen = 0;
    uint32_t chars_seen = 0;
    bool primed = false;

    static ImGuiKey key_of(unsigned vk) {
        if (vk >= 'A' && vk <= 'Z') return ImGuiKey(ImGuiKey_A + int(vk - 'A'));
        if (vk >= '0' && vk <= '9') return ImGuiKey(ImGuiKey_0 + int(vk - '0'));
        if (vk >= 0x70 && vk <= 0x87) return ImGuiKey(ImGuiKey_F1 + int(vk - 0x70));
        switch (vk) {
            case 0x13: return ImGuiKey_Pause; case 0x91: return ImGuiKey_ScrollLock;
            case 0x08: return ImGuiKey_Backspace; case 0x09: return ImGuiKey_Tab; case 0x0D: return ImGuiKey_Enter;
            case 0x1B: return ImGuiKey_Escape; case 0x20: return ImGuiKey_Space; case 0x21: return ImGuiKey_PageUp;
            case 0x22: return ImGuiKey_PageDown; case 0x23: return ImGuiKey_End; case 0x24: return ImGuiKey_Home;
            case 0x25: return ImGuiKey_LeftArrow; case 0x26: return ImGuiKey_UpArrow; case 0x27: return ImGuiKey_RightArrow;
            case 0x28: return ImGuiKey_DownArrow; case 0x2D: return ImGuiKey_Insert; case 0x2E: return ImGuiKey_Delete;
            case 0xA0: return ImGuiKey_LeftShift; case 0xA1: return ImGuiKey_RightShift;
            case 0xA2: return ImGuiKey_LeftCtrl; case 0xA3: return ImGuiKey_RightCtrl;
            case 0xA4: return ImGuiKey_LeftAlt; case 0xA5: return ImGuiKey_RightAlt;
            default: return ImGuiKey_None;
        }
    }

    void update(ImGuiIO& io, uint32_t width, uint32_t height) {
        auto* s = shared();
        s->wayland = 1;
        auto down = [&](unsigned vk) { return (s->keys[vk / 32] >> (vk % 32)) & 1u; };
        auto was = [&](unsigned vk) { return (keys_before[vk / 32] >> (vk % 32)) & 1u; };
        if (!primed) {
            std::memcpy(keys_before, s->keys, sizeof keys_before);
            wheel_seen = s->wheel;
            chars_seen = s->chars_written;
            primed = true;
        }
        const bool mods_held = (!(open_mods & MOD_CONTROL) || down(0x11)) &&
                               (!(open_mods & MOD_SHIFT) || down(0x10)) &&
                               (!(open_mods & MOD_ALT) || down(0x12));
        bool toggled = false;
        if (open_vk && down(unsigned(open_vk)) && !was(unsigned(open_vk)) && mods_held && !(open && io.WantTextInput)) {
            open = !open;
            toggled = true;
        }
        if (open && down(0x1B) && !was(0x1B) && !io.WantTextInput) open = false;
        s->menu_open = open ? 1 : 0;
        io.MouseDrawCursor = open && !s->cursor_shown;
        if (open) {
            const float sx = s->client_w ? float(width) / float(s->client_w) : 1.f;
            const float sy = s->client_h ? float(height) / float(s->client_h) : 1.f;
            io.AddMousePosEvent(float(s->mouse_x) * sx, float(s->mouse_y) * sy);
            for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, (s->buttons >> b) & 1u);
            const int32_t wheel = s->wheel;
            if (wheel != wheel_seen) io.AddMouseWheelEvent(0.f, float(wheel - wheel_seen) / 120.f);
            for (unsigned vk = 8; vk < 256; ++vk)
                if (down(vk) != was(vk) && !(toggled && vk == unsigned(open_vk)))
                    if (ImGuiKey k = key_of(vk); k != ImGuiKey_None) io.AddKeyEvent(k, down(vk) != 0);
            io.AddKeyEvent(ImGuiMod_Ctrl, down(0x11) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, down(0x10) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, down(0x12) != 0);
            for (uint32_t c = chars_seen; c != s->chars_written; ++c) io.AddInputCharacter(s->chars[c % 64]);
        }
        wheel_seen = s->wheel;
        chars_seen = s->chars_written;
        std::memcpy(keys_before, s->keys, sizeof keys_before);
    }

    void close() {
        open = false;
        shared()->menu_open = 0;
    }
};

Input g_input;
std::string g_api_name;
thread_local bool t_same_frame = false;
std::atomic<ULONGLONG> g_game_present_ms{0};
std::atomic<bool> g_game_frame_new{false};

struct Menu {
    ImGuiContext* context{};
    std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();
    Ini ini{game_dir() / "openrr-amd.ini"};
    int64_t loaded = -1;

    Menu() {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDebugHighlightIdConflicts = false;
        apply_style(1.f);
    }

    ~Menu() {
        if (context) ImGui::DestroyContext(context);
    }

    static constexpr int kThemes = 6;
    static constexpr const char* kThemeNames[kThemes] = {"Blue", "Teal", "Violet", "Amber", "Crimson", "Graphite"};

    static ImVec4 theme_accent(int theme) {
        static const ImVec4 accents[kThemes] = {
            {0.00f, 0.40f, 0.77f, 1.f},
            {0.08f, 0.62f, 0.58f, 1.f},
            {0.52f, 0.36f, 0.86f, 1.f},
            {0.86f, 0.56f, 0.10f, 1.f},
            {0.80f, 0.20f, 0.26f, 1.f},
            {0.50f, 0.54f, 0.60f, 1.f}};
        return accents[std::clamp(theme, 0, kThemes - 1)];
    }

    static void apply_style(float scale, int theme = 0) {
        const ImVec4 accent = theme_accent(theme);
        const auto mix = [](const ImVec4& a, const ImVec4& b, float t, float alpha = 1.f) {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha);
        };
        const ImVec4 bg_dark(0.09f, 0.09f, 0.10f, 1.f), bg_mid(0.11f, 0.11f, 0.12f, 1.f), bg_light(0.14f, 0.14f, 0.15f, 1.f);
        const ImVec4 black(0.f, 0.f, 0.f, 1.f), text(0.90f, 0.93f, 0.95f, 1.f), dim(0.54f, 0.58f, 0.62f, 1.f);
        const ImVec4 border(0.24f, 0.24f, 0.26f, 1.f);
        const auto tint = [&](const ImVec4& base, float strength = 1.f, float alpha = 1.f) {
            return mix(base, black, 0.12f * strength, alpha);
        };
        const auto soft = [&](float a = 1.f) { return mix(bg_dark, accent, 0.32f, a); };
        const auto med = [&](float a = 1.f) { return mix(bg_dark, accent, 0.55f, a); };
        const auto strong = [&](float a = 1.f) { return ImVec4(accent.x, accent.y, accent.z, a); };
        const ImVec4 readable = mix(accent, ImVec4(1.f, 1.f, 1.f, 1.f), 0.25f);
        ImGuiStyle st;
        ImGui::StyleColorsDark(&st);
        st.WindowRounding = 2; st.ChildRounding = 2; st.FrameRounding = 2; st.PopupRounding = 2;
        st.ScrollbarRounding = 2; st.GrabRounding = 2; st.TabRounding = 2;
        st.WindowBorderSize = 1; st.ChildBorderSize = 1; st.PopupBorderSize = 1; st.FrameBorderSize = 0;
        st.ScrollbarSize = 10; st.GrabMinSize = 10; st.SeparatorTextBorderSize = 1;
        st.WindowPadding = {8, 8}; st.FramePadding = {6, 3}; st.ItemSpacing = {8, 5}; st.ItemInnerSpacing = {6, 4};
        st.WindowTitleAlign = {0.0f, 0.5f};
        ImVec4* c = st.Colors;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = dim;
        c[ImGuiCol_WindowBg] = tint(bg_dark, 1.f, 0.93f);
        c[ImGuiCol_ChildBg] = tint(bg_mid, 1.1f, 0.35f);
        c[ImGuiCol_PopupBg] = tint(bg_dark, 1.f, 0.98f);
        c[ImGuiCol_Border] = border;
        c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
        c[ImGuiCol_FrameBg] = tint(bg_light, 0.5f, 0.6f);
        c[ImGuiCol_FrameBgHovered] = mix(bg_light, accent, 0.18f);
        c[ImGuiCol_FrameBgActive] = mix(bg_light, accent, 0.28f);
        c[ImGuiCol_TitleBg] = soft(0.85f);
        c[ImGuiCol_TitleBgActive] = mix(soft(), accent, 0.16f);
        c[ImGuiCol_TitleBgCollapsed] = soft(0.75f);
        c[ImGuiCol_ScrollbarBg] = tint(bg_dark, 0.6f, 0.4f);
        c[ImGuiCol_ScrollbarGrab] = soft();
        c[ImGuiCol_ScrollbarGrabHovered] = med();
        c[ImGuiCol_ScrollbarGrabActive] = strong();
        c[ImGuiCol_CheckMark] = readable;
        c[ImGuiCol_SliderGrab] = med();
        c[ImGuiCol_SliderGrabActive] = readable;
        c[ImGuiCol_Button] = soft();
        c[ImGuiCol_ButtonHovered] = med();
        c[ImGuiCol_ButtonActive] = strong();
        c[ImGuiCol_Header] = soft(0.9f);
        c[ImGuiCol_HeaderHovered] = med(0.95f);
        c[ImGuiCol_HeaderActive] = strong();
        c[ImGuiCol_Separator] = border;
        c[ImGuiCol_SeparatorHovered] = med(0.85f);
        c[ImGuiCol_SeparatorActive] = strong();
        c[ImGuiCol_ResizeGrip] = soft(0.3f);
        c[ImGuiCol_ResizeGripHovered] = strong(0.7f);
        c[ImGuiCol_ResizeGripActive] = strong(0.95f);
        c[ImGuiCol_Tab] = tint(bg_dark, 0.6f);
        c[ImGuiCol_TabHovered] = med(0.85f);
        c[ImGuiCol_TabSelected] = med();
        c[ImGuiCol_TableHeaderBg] = tint(bg_mid, 0.8f, 0.6f);
        c[ImGuiCol_TableBorderStrong] = border;
        c[ImGuiCol_TableBorderLight] = soft();
        c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
        c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.03f};
        c[ImGuiCol_TextSelectedBg] = med(0.38f);
        c[ImGuiCol_NavCursor] = readable;
        c[ImGuiCol_PlotHistogram] = mix(accent, ImVec4(1, 1, 1, 1), 0.35f, 0.85f);
        st.ScaleAllSizes(scale);
        ImGui::GetStyle() = st;
    }

    int theme() const {
        return std::clamp(openrr::parse_integer<int>(ini.get("OpenRR", "Theme", "0"), 0), 0, kThemes - 1);
    }

    float font_px = 0.f;
    float style_scale = 1.f;
    float window_scale = 0.f;
    ImVec2 window_size{};
    int theme_now = -1;

    bool fonts_for(uint32_t height) {
        if (const int t = theme(); t != theme_now && font_px > 0.f) { theme_now = t; apply_style(style_scale, t); }
        float scale = openrr::parse_float(ini.get("OpenRR", "MenuScale", "1"), 1.0f);
        if (!(scale >= 0.5f && scale <= 3.f)) scale = 1.f;
        float text = openrr::parse_float(ini.get("OpenRR", "TextScale", "1"), 1.0f);
        if (!(text >= 0.75f && text <= 2.f)) text = 1.f;
        const float menu_px = std::clamp(std::round(float(height) / 1080.f * 17.f * scale), 12.f, 72.f);
        const float px = std::clamp(std::round(menu_px * text), 9.f, 144.f);
        if (px == font_px) return false;
        font_px = px;
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFont), int(kMenuFontSize), px, &cfg);
        theme_now = theme();
        style_scale = menu_px / 17.f;
        apply_style(style_scale, theme_now);
        return true;
    }

    // [OpenRR] MenuKey: default "optiscaler" follows OptiScaler.ini [Menu] ShortcutKey (Insert by default).
    std::string menu_key_seen = "\x01";
    int opti_vk_seen = -1;
    ULONGLONG menu_key_read = 0;
    void menu_key() {
        const ULONGLONG now = GetTickCount64();
        if (!g_input.open && menu_key_read && now - menu_key_read < 1000) return;
        menu_key_read = now;

        const std::string folder = openrr::pe::module_folder();
        std::string text;
        if (g_input.open) {
            text = ini.get("OpenRR", "MenuKey", "optiscaler");
        } else {
            char v[64] = {};
            const std::string path = folder + "\\openrr-amd.ini";
            openrr::u8::profile("OpenRR", "MenuKey", "optiscaler", v, sizeof v, path);
            text = v;
        }
        int opti_mods = 0;
        const int opti_vk = openrr::pe::read_optiscaler_shortcut_key(folder, &opti_mods);
        if (text == menu_key_seen && opti_vk == opti_vk_seen) return;
        menu_key_seen = text;
        opti_vk_seen = opti_vk;

        std::string lower_t = text;
        for (char& c : lower_t) c = char(std::tolower(static_cast<unsigned char>(c)));
        const auto a = lower_t.find_first_not_of(" \t"), b = lower_t.find_last_not_of(" \t");
        lower_t = a == std::string::npos ? std::string() : lower_t.substr(a, b - a + 1);

        int vk = 0, mods = 0;
        if (lower_t.empty() || lower_t == "none" || lower_t == "off") {
            log("[openrr] OpenRR menu: [OpenRR] MenuKey is off; no key opens the menu");
        } else if (lower_t == "optiscaler" || lower_t == "auto") {
            vk = opti_vk;
            mods = opti_mods;
            log("[openrr] OpenRR menu: following OptiScaler ShortcutKey (%s, 0x%02X)",
                openrr::pe::vk_name(vk, mods).c_str(), vk);
        } else if (!openrr::pe::parse_key(text, &vk, &mods)) {
            vk = opti_vk;
            mods = opti_mods;
            log("[openrr] OpenRR menu: [OpenRR] MenuKey '%s' is invalid; following OptiScaler ShortcutKey (%s)",
                text.c_str(), openrr::pe::vk_name(vk, mods).c_str());
        } else {
            log("[openrr] OpenRR menu: %s opens the menu ([OpenRR] MenuKey)", openrr::pe::vk_name(vk, mods).c_str());
        }
        g_input.open_vk = vk;
        g_input.open_mods = mods;
    }

    bool input(uint32_t width, uint32_t height) {
        ++shared()->menu_seq;
        ++shared()->dll_menu;
        auto& io = ImGui::GetIO();
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::clamp(std::chrono::duration<float>(now - previous).count(), .001f, .1f);
        previous = now;
        io.DisplaySize = {float(width), float(height)};
        menu_key();
        g_input.update(io, width, height);
        return g_input.open;
    }

    static bool bool_val(const std::string& s, bool def) {
        std::string l = s;
        for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (l == "1" || l == "true" || l == "on" || l == "yes") return true;
        if (l == "0" || l == "false" || l == "off" || l == "no") return false;
        return def;
    }

    bool checkbox_ini(const char* label, const char* key, bool def) {
        bool v = bool_val(ini.get("OpenRR", key, def ? "true" : "false"), def);
        if (ImGui::Checkbox(label, &v)) {
            ini.set("OpenRR", key, v ? "true" : "false");
            return true;
        }
        return false;
    }

    void draw() {
        const openrr::control::Shared* s = shared();
        const std::string gpu = openrr::pe::vkdevice::gpu_name();
        ImGui::TextDisabled("%s%s%s",
                            g_api_name.empty() ? "API" : g_api_name.c_str(),
                            gpu.empty() ? "" : "  |  ", gpu.c_str());
        ImGui::Separator();

        checkbox_ini("Enable OpenRR (Ray Reconstruction)", "Enabled", true);
        ImGui::SameLine();
        ImGui::TextDisabled("(Menu key: %s)", openrr::pe::vk_name(g_input.open_vk, g_input.open_mods).c_str());

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s || !s->rr_active) {
                ImGui::TextWrapped("Idle: waiting for the game to create a DLSS Ray Reconstruction feature.");
                if (s && s->message[0]) {
                    ImGui::TextDisabled("Note: %s", s->message);
                }
            } else {
                const char* st_name = s->state == ONR_RR_STATE_READY      ? "Ready"
                                    : s->state == ONR_RR_STATE_BUILDING   ? "Building network..."
                                    : s->state == ONR_RR_STATE_FAILED     ? "Failed"
                                                                          : "Unknown";
                const char* be_name = s->backend == ONR_RR_BACKEND_D3D12       ? "D3D12 Direct ELF"
                                    : s->backend == ONR_RR_BACKEND_VULKAN_HOST ? "Vulkan Host (RecordVk)"
                                                                               : "Vulkan Interop Bridge";
                ImGui::Text("State: %s%s%s%s", st_name,
                            s->message[0] ? " (" : "", s->message, s->message[0] ? ")" : "");
                ImGui::Text("Backend: %s", be_name);
                if (s->backend_message[0]) {
                    ImGui::TextDisabled("Backend note: %s", s->backend_message);
                }
                if (s->render_width && s->output_width) {
                    ImGui::Text("Resolution: %ux%u -> %ux%u",
                                s->render_width, s->render_height, s->output_width, s->output_height);
                }
                ImGui::Text("GPU time (raw): %.2f ms (p50 %.2f ms, p95 %.2f ms)  |  Frames: %u",
                            double(s->gpu_raw_last), double(s->gpu_raw_p50), double(s->gpu_raw_p95), s->frames);
                ImGui::TextDisabled("Est. w/o async preemption: %.2f ms (avg %.2f ms)",
                                    double(s->gpu_ms_last), double(s->gpu_ms_average));
                if (s->weights_source[0]) {
                    ImGui::TextWrapped("Weights: %s", s->weights_source);
                }
            }
            if (s && s->coexist_mochi) {
                ImGui::TextDisabled("Coexistence: dlssnr-amd.asi detected (Direct path active)");
            }
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Execution & Backend", ImGuiTreeNodeFlags_DefaultOpen)) {
            const std::string be = ini.get("OpenRR", "Backend", "auto");
            int be_idx = (be == "d3d12") ? 1 : (be == "vulkan") ? 2 : 0;
            const char* be_items[] = {"Auto (D3D12 Direct preferred)", "D3D12 Direct ELF", "Vulkan Interop Bridge"};
            if (ImGui::Combo("D3D12 Backend", &be_idx, be_items, 3)) {
                ini.set("OpenRR", "Backend", be_idx == 1 ? "d3d12" : be_idx == 2 ? "vulkan" : "auto");
            }

            const std::string mode = ini.get("OpenRR", "Mode", "new");
            int mode_idx = (mode == "legacy" || mode == "0") ? 1 : 0;
            const char* mode_items[] = {"Fast (_lt kernels, recommended)", "Legacy (full-precision _f kernels)"};
            if (ImGui::Combo("Network Mode", &mode_idx, mode_items, 2)) {
                ini.set("OpenRR", "Mode", mode_idx == 1 ? "legacy" : "new");
            }

            const std::string cs = ini.get("OpenRR", "CoexistSplit", "auto");
            int cs_idx = (cs == "off" || cs == "0" || cs == "false") ? 1 : 0;
            const char* cs_items[] = {"Auto (disable split when dlssnr-amd is present)", "Off (never split command lists)"};
            if (ImGui::Combo("Coexist Split", &cs_idx, cs_items, 2)) {
                ini.set("OpenRR", "CoexistSplit", cs_idx == 1 ? "off" : "auto");
            }

            const std::string hw = ini.get("OpenRR", "HardwareDepth", "auto");
            int hw_idx = (hw == "true" || hw == "1") ? 1 : (hw == "false" || hw == "0") ? 2 : 0;
            const char* hw_items[] = {"Auto", "Hardware Depth (NDC)", "Linear Depth"};
            if (ImGui::Combo("Depth Type", &hw_idx, hw_items, 3)) {
                ini.set("OpenRR", "HardwareDepth", hw_idx == 1 ? "true" : hw_idx == 2 ? "false" : "auto");
            }

            const std::string vkl = ini.get("OpenRR", "VkInputLayout", "shader_read");
            int vkl_idx = (vkl == "general") ? 1 : 0;
            const char* vkl_items[] = {"Shader Read Optimal", "General"};
            if (ImGui::Combo("Vulkan Input Layout", &vkl_idx, vkl_items, 2)) {
                ini.set("OpenRR", "VkInputLayout", vkl_idx == 1 ? "general" : "shader_read");
            }
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Guides")) {
            checkbox_ini("Use Disocclusion Mask", "UseDisocclusionMask", true);
            checkbox_ini("Invert Disocclusion Mask", "InvertDisocclusionMask", false);
            checkbox_ini("Clamp Disocclusion Mask (0..1)", "ClampDisocclusionMask", true);
            float dm_scale = openrr::parse_float(ini.get("OpenRR", "DisocclusionMaskScale", "1.0"), 1.0f);
            if (ImGui::SliderFloat("Disocclusion Mask Scale", &dm_scale, 0.1f, 4.0f, "%.2f")) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.2f", double(dm_scale));
                ini.set("OpenRR", "DisocclusionMaskScale", buf);
            }
            ImGui::Separator();
            checkbox_ini("Use SSS Guide (ColorBeforeFog)", "UseSssGuide", true);
            checkbox_ini("Use Responsivity Mask", "UseResponsivityMask", false);
            checkbox_ini("Use Color Before Transparency", "UseColorBeforeTransparency", false);
            checkbox_ini("Use Depth of Field Guide", "UseDepthOfFieldGuide", false);
            checkbox_ini("Use Reflection Motion Vectors", "UseReflectionMotionVectors", false);
            checkbox_ini("Use Transparency Layer Opacity & Motion", "UseLayerOpacityAndMotion", false);
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Menu & Hotkeys")) {
            checkbox_ini("Pass OptiScaler Key (Insert) while open", "PassOptiScalerKey", true);
            float mscale = openrr::parse_float(ini.get("OpenRR", "MenuScale", "1.0"), 1.0f);
            if (ImGui::SliderFloat("Menu Size", &mscale, 0.75f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.2f", double(mscale));
                ini.set("OpenRR", "MenuScale", buf);
            }
            float tscale = openrr::parse_float(ini.get("OpenRR", "TextScale", "1.0"), 1.0f);
            if (ImGui::SliderFloat("Text Size", &tscale, 0.75f, 1.75f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.2f", double(tscale));
                ini.set("OpenRR", "TextScale", buf);
            }
            int th = theme();
            if (ImGui::Combo("Theme", &th, kThemeNames, kThemes)) {
                ini.set("OpenRR", "Theme", std::to_string(th));
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close")) g_input.close();
    }

    void build(uint32_t height) {
        if (ini.refresh() || loaded < 0) loaded = loaded < 0 ? 1 : loaded + 1;
        ImGui::NewFrame();
        const float u = style_scale;
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        // Docked beside the right edge of OptiScaler's centered menu panel on first use (Item #14)
        const float opti_s = openrr::pe::read_optiscaler_menu_scale(openrr::pe::module_folder(), ds.y);
        const float opti_w = std::min(680.f * opti_s, ds.x - 20.f);
        const float our_w = std::min(460.f * u, ds.x - 48.f);
        float dock_x = (ds.x + opti_w) * 0.5f + 8.f * u;
        if (dock_x + our_w > ds.x)
            dock_x = std::max(8.f * u, (ds.x - opti_w) * 0.5f - 8.f * u - our_w);
        ImGui::SetNextWindowPos({dock_x, 24.f * u}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({our_w, std::min(float(height) - 48.f, 640.f * u)},
                                 ImGuiCond_FirstUseEver);
        if (window_scale > 0.f && window_scale != u && window_size.x > 0.f) {
            const float r = u / window_scale;
            ImGui::SetNextWindowSize({std::min(window_size.x * r, ds.x - 48.f), std::min(window_size.y * r, ds.y - 48.f)},
                                     ImGuiCond_Always);
        }
        window_scale = u;
        bool open = true;
        char title[160];
        const openrr::control::Shared* sh = shared();
        const char* build_str = (sh && sh->asi_build[0]) ? sh->asi_build : OPENRR_BUILD_STAMP;
        std::snprintf(title, sizeof title, "OpenRR %s (%s)###OpenRR", ONR_RR_RUNTIME_VERSION, build_str);
        if (ImGui::Begin(title, &open)) {
            draw();
            window_size = ImGui::GetWindowSize();
        }
        ImGui::End();
        ImGui::Render();
        if (!open) g_input.close();
        ini.save();
    }
};

// ---- Composite shader over the back buffer ----
const char kComposeHlsl[] = R"(
Texture2D<float4> menu : register(t0);
cbuffer Push : register(b0) { uint mode; float scale; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
float3 srgb_to_linear(float3 c) {
    return c > 0.04045 ? pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
}
float3 pq(float3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(max(n, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 m = menu.Load(int3(int2(pos.xy), 0));
    if (m.a <= 0.0) discard;
    float3 c = saturate(m.rgb / m.a);
    if (mode == 1u) {
        c = srgb_to_linear(c);
    } else if (mode == 2u) {
        c = srgb_to_linear(c) * scale;
    } else if (mode == 3u) {
        float3 l = srgb_to_linear(c);
        float3 w = float3(dot(float3(0.627404, 0.329283, 0.043313), l),
                          dot(float3(0.069097, 0.919540, 0.011362), l),
                          dot(float3(0.016391, 0.088013, 0.895595), l));
        c = pq(w * scale);
    }
    return float4(c * m.a, m.a);
}
)";

struct Encoding { uint32_t mode; float scale; const char* name; };

std::mutex g_spaces_mutex;
std::unordered_map<IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE> g_spaces;
int g_last_space = -1;

Encoding encoding_of(IDXGISwapChain* sc, DXGI_FORMAT format) {
    constexpr float kWhiteNits = 203.f;
    bool known = false;
    DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        const auto it = g_spaces.find(sc);
        if (it != g_spaces.end()) { space = it->second; known = true; }
        else if (g_last_space >= 0) { space = DXGI_COLOR_SPACE_TYPE(g_last_space); known = true; }
    }
    if (!known) {
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            IDXGIOutput* out = nullptr;
            IDXGIOutput6* out6 = nullptr;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(sc->GetContainingOutput(&out)) && out && SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out6))) &&
                out6 && SUCCEEDED(out6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            release(out6);
            release(out);
        }
    }
    if (space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) return {2, kWhiteNits / 80.f, "scRGB, white at 203 nits"};
    if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return {3, kWhiteNits / 10000.f, "HDR10 (PQ), white at 203 nits"};
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return {1, 1.f, "sRGB format"};
        default: return {0, 1.f, "SDR"};
    }
}

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kComposeHlsl, sizeof kComposeHlsl - 1, "rr_menu_compose", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    release(errors);
    if (FAILED(hr) || !code) throw std::runtime_error("the composite shader " + std::string(entry) + " did not compile: " + why);
    return code;
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char t[160];
        std::snprintf(t, sizeof t, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(t);
    }
}

// ---- D3D11 ----
struct D3D11Overlay {
    ID3D11Device* device{};
    ID3D11Device1* device1{};
    ID3D11DeviceContext* context{};
    ID3D11DeviceContext1* context1{};
    ID3DDeviceContextState* state{};
    ID3D11Texture2D* menu{};
    ID3D11RenderTargetView* menu_rtv{};
    ID3D11ShaderResourceView* menu_srv{};
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    ID3D11BlendState* blend{};
    ID3D11RasterizerState* raster{};
    ID3D11Buffer* constants{};
    uint32_t width{}, height{};
    Menu menu_ui;
    bool backend = false;

    explicit D3D11Overlay(ID3D11Device* d) : device(d) {
        device->AddRef();
        device->GetImmediateContext(&context);
        check(device->QueryInterface(IID_PPV_ARGS(&device1)), "ID3D11Device1");
        check(context->QueryInterface(IID_PPV_ARGS(&context1)), "ID3D11DeviceContext1");
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                &state), "CreateDeviceContextState");
        ID3DBlob* v = compile("vs", "vs_5_0");
        const HRESULT vr = device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs);
        v->Release();
        check(vr, "the composite vertex shader");
        ID3DBlob* p = compile("ps", "ps_5_0");
        const HRESULT pr = device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps);
        p->Release();
        check(pr, "the composite pixel shader");
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&bd, &blend), "the blend state");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rd, &raster), "the rasterizer state");
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cd, nullptr, &constants), "the constant buffer");
        ImGui::SetCurrentContext(menu_ui.context);
        ID3DDeviceContextState* game = nullptr;
        context1->SwapDeviceContextState(state, &game);
        backend = ImGui_ImplDX11_Init(device, context);
        context1->SwapDeviceContextState(game, nullptr);
        release(game);
        if (!backend) throw std::runtime_error("ImGui's D3D11 backend did not start");
    }

    ~D3D11Overlay() {
        ImGui::SetCurrentContext(menu_ui.context);
        if (backend) ImGui_ImplDX11_Shutdown();
        free_menu();
        release(constants); release(raster); release(blend); release(ps); release(vs);
        release(state); release(context1); release(context); release(device1); release(device);
    }

    void free_menu() { release(menu_srv); release(menu_rtv); release(menu); }

    void ensure_menu(uint32_t w, uint32_t h) {
        if (menu && w == width && h == height) return;
        free_menu();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&td, nullptr, &menu), "the menu image");
        check(device->CreateRenderTargetView(menu, nullptr, &menu_rtv), "the menu's render target");
        check(device->CreateShaderResourceView(menu, nullptr, &menu_srv), "the menu's view");
        width = w; height = h;
    }

    void present(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        check(sc->GetDesc(&desc), "GetDesc");
        const uint32_t w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
        if (!w || !h) return;
        ImGui::SetCurrentContext(menu_ui.context);
        if (!menu_ui.input(w, h)) return;
        ID3D11Texture2D* back = nullptr;
        check(sc->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
        ID3DDeviceContextState* game = nullptr;
        context1->SwapDeviceContextState(state, &game);
        ID3D11RenderTargetView* rtv = nullptr;
        try {
            ensure_menu(w, h);
            if (menu_ui.fonts_for(h)) ImGui_ImplDX11_InvalidateDeviceObjects();
            ImGui_ImplDX11_NewFrame();
            menu_ui.build(h);
            const float clear[4] = {0, 0, 0, 0};
            context->ClearRenderTargetView(menu_rtv, clear);
            context->OMSetRenderTargets(1, &menu_rtv, nullptr);
            D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            context->RSSetViewports(1, &vp);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            check(device->CreateRenderTargetView(back, nullptr, &rtv), "the back buffer's render target");
            const Encoding enc = encoding_of(sc, desc.BufferDesc.Format);
            const struct { uint32_t mode; float scale; uint32_t pad[2]; } push{enc.mode, enc.scale, {}};
            context->UpdateSubresource(constants, 0, nullptr, &push, 0, 0);
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->OMSetBlendState(blend, nullptr, 0xffffffffu);
            context->OMSetDepthStencilState(nullptr, 0);
            context->RSSetState(raster);
            context->RSSetViewports(1, &vp);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->PSSetShaderResources(0, 1, &menu_srv);
            context->PSSetConstantBuffers(0, 1, &constants);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            context->PSSetShaderResources(0, 1, &none);
            ++g_drawn;
        } catch (...) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            release(rtv);
            context1->SwapDeviceContextState(game, nullptr);
            release(game);
            release(back);
            throw;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        release(rtv);
        context1->SwapDeviceContextState(game, nullptr);
        release(game);
        release(back);
    }
};

// ---- D3D12 ----
struct D3D12Overlay {
    static constexpr uint32_t kFrames = 6, kSrvs = 64;
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    ID3D12CommandAllocator* allocators[kFrames]{};
    uint64_t done_at[kFrames]{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence* fence{};
    uint64_t fence_value{};
    HANDLE event{};
    uint32_t frame{};
    ID3D12DescriptorHeap* rtvs{};
    ID3D12DescriptorHeap* srvs{};
    uint32_t rtv_step{}, srv_step{};
    std::vector<bool> srv_used = std::vector<bool>(kSrvs, false);
    ID3D12Resource* menu{};
    uint32_t width{}, height{};
    ID3D12RootSignature* root{};
    std::map<DXGI_FORMAT, ID3D12PipelineState*> pipelines;
    ID3DBlob* vs_code{};
    ID3DBlob* ps_code{};
    Menu menu_ui;
    bool backend = false;

    D3D12Overlay(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        device->AddRef();
        queue->AddRef();
        for (auto& a : allocators)
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "a command allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&list)),
              "the command list");
        list->Close();
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "the fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "the render-target heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSrvs;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs)), "the shader-resource heap");
        rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srv_used[0] = true;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 2;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2; rd.pParameters = params;
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT sr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        release(errors);
        check(sr, "the root signature's serialisation");
        const HRESULT rr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        blob->Release();
        check(rr, "the root signature");
        vs_code = compile("vs", "vs_5_0");
        ps_code = compile("ps", "ps_5_0");
        ImGui::SetCurrentContext(menu_ui.context);
        ImGui_ImplDX12_InitInfo init;
        init.Device = device;
        init.CommandQueue = queue;
        init.NumFramesInFlight = int(kFrames);
        init.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init.UserData = this;
        init.SrvDescriptorHeap = srvs;
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto* o = static_cast<D3D12Overlay*>(info->UserData);
            for (uint32_t i = 1; i < kSrvs; ++i)
                if (!o->srv_used[i]) {
                    o->srv_used[i] = true;
                    *cpu = o->srv_cpu(i);
                    *gpu = o->srv_gpu(i);
                    return;
                }
            *cpu = o->srv_cpu(kSrvs - 1);
            *gpu = o->srv_gpu(kSrvs - 1);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {
            auto* o = static_cast<D3D12Overlay*>(info->UserData);
            const uint32_t i = uint32_t((cpu.ptr - o->srvs->GetCPUDescriptorHandleForHeapStart().ptr) / o->srv_step);
            if (i < kSrvs) o->srv_used[i] = false;
        };
        backend = ImGui_ImplDX12_Init(&init);
        if (!backend) throw std::runtime_error("ImGui's D3D12 backend did not start");
    }

    ~D3D12Overlay() {
        if (!wait_all()) {
            say("the menu's last list did not finish in 2 s; its objects are kept, not freed");
            return;
        }
        ImGui::SetCurrentContext(menu_ui.context);
        if (backend) ImGui_ImplDX12_Shutdown();
        for (auto& p : pipelines) p.second->Release();
        release(vs_code); release(ps_code); release(root); release(menu); release(srvs); release(rtvs);
        if (event) CloseHandle(event);
        release(fence); release(list);
        for (auto& a : allocators) release(a);
        release(queue); release(device);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * srv_step;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(uint32_t i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvs->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(i) * srv_step;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtv_step;
        return h;
    }

    bool wait(uint64_t value) {
        if (!value || fence->GetCompletedValue() >= value) return true;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) WaitForSingleObject(event, 2000);
        return fence->GetCompletedValue() >= value;
    }
    bool wait_all() { return !fence || wait(fence_value); }

    bool ensure_menu(uint32_t w, uint32_t h) {
        if (menu && w == width && h == height) return true;
        if (!wait_all()) return false;
        release(menu);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              &clear, IID_PPV_ARGS(&menu)), "the menu image");
        device->CreateRenderTargetView(menu, nullptr, rtv(0));
        device->CreateShaderResourceView(menu, nullptr, srv_cpu(0));
        width = w; height = h;
        return true;
    }

    ID3D12PipelineState* pipeline(DXGI_FORMAT format) {
        const auto it = pipelines.find(format);
        if (it != pipelines.end()) return it->second;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {vs_code->GetBufferPointer(), vs_code->GetBufferSize()};
        pd.PS = {ps_code->GetBufferPointer(), ps_code->GetBufferSize()};
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = format;
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState* p = nullptr;
        check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)), "the composite pipeline");
        pipelines[format] = p;
        return p;
    }

    static void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                           D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        l->ResourceBarrier(1, &b);
    }

    void present(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        check(sc->GetDesc(&desc), "GetDesc");
        const uint32_t w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
        if (!w || !h) return;
        ImGui::SetCurrentContext(menu_ui.context);
        if (!menu_ui.input(w, h)) return;
        IDXGISwapChain3* sc3 = nullptr;
        check(sc->QueryInterface(IID_PPV_ARGS(&sc3)), "IDXGISwapChain3");
        const UINT index = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        ID3D12Resource* back = nullptr;
        check(sc->GetBuffer(index, IID_PPV_ARGS(&back)), "GetBuffer");
        struct Hold { ID3D12Resource*& r; ~Hold() { release(r); } } hold{back};
        if (!ensure_menu(w, h)) return;
        const Encoding enc = encoding_of(sc, desc.BufferDesc.Format);
        ID3D12PipelineState* compose = pipeline(desc.BufferDesc.Format);
        if (!wait(done_at[frame])) return;
        if (menu_ui.fonts_for(h)) {
            if (!wait_all()) return;
            ImGui_ImplDX12_InvalidateDeviceObjects();
        }
        ImGui_ImplDX12_NewFrame();
        menu_ui.build(h);
        check(allocators[frame]->Reset(), "the command allocator's reset");
        check(list->Reset(allocators[frame], nullptr), "the command list's reset");
        transition(list, menu, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0, 0, 0, 0};
        const D3D12_CPU_DESCRIPTOR_HANDLE menu_rtv = rtv(0), back_rtv = rtv(1);
        list->ClearRenderTargetView(menu_rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &menu_rtv, FALSE, nullptr);
        list->SetDescriptorHeaps(1, &srvs);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        transition(list, menu, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        device->CreateRenderTargetView(back, nullptr, back_rtv);
        transition(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        list->SetPipelineState(compose);
        list->SetGraphicsRootSignature(root);
        list->SetDescriptorHeaps(1, &srvs);
        list->SetGraphicsRootDescriptorTable(0, srv_gpu(0));
        const uint32_t push[2] = {enc.mode, 0};
        list->SetGraphicsRoot32BitConstant(1, push[0], 0);
        uint32_t scale_bits;
        std::memcpy(&scale_bits, &enc.scale, 4);
        list->SetGraphicsRoot32BitConstant(1, scale_bits, 1);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        ++g_drawn;
        transition(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        check(list->Close(), "the command list's close");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        done_at[frame] = fence_value + 1;
        check(queue->Signal(fence, ++fence_value), "the fence signal");
        frame = (frame + 1) % kFrames;
    }
};

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn g_execute = nullptr;
std::atomic<ExecuteObserver> g_execute_observer{nullptr};
thread_local ID3D12CommandQueue* t_last_direct = nullptr;
thread_local ID3D12CommandQueue* t_last_seen = nullptr;

void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (q != t_last_seen) {
        t_last_seen = q;
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_last_direct = q;
    }
    g_execute(q, n, lists);
    if (const ExecuteObserver observer = g_execute_observer.load()) observer(q, n, lists);
}

ID3D12CommandQueue* last_direct_queue() { return t_last_direct; }
std::atomic<D3D12PresentHook> g_d3d12_hook{nullptr};
std::atomic<D3D11PresentHook> g_d3d11_hook{nullptr};
std::atomic<D3D12ShownHook> g_d3d12_shown{nullptr};

std::mutex g_mutex;
std::atomic<uint64_t> g_presents{0};
template <class T> struct Kept { std::unique_ptr<T> overlay; std::chrono::steady_clock::time_point used; };
std::map<void*, Kept<D3D11Overlay>> g_d3d11;
std::map<void*, Kept<D3D12Overlay>> g_d3d12;
constexpr std::chrono::seconds kIdle{3};
IDXGISwapChain* g_failed_for = nullptr;
thread_local bool t_inside = false;

template <class T> void let_go_idle(std::map<void*, Kept<T>>& m, std::chrono::steady_clock::time_point now) {
    for (auto it = m.begin(); it != m.end();)
        if (now - it->second.used > kIdle) it = m.erase(it);
        else ++it;
}

bool dump_wanted() { return false; }

#include "rr_pe_menu_vk.inl"

struct CreatedQueue { IDXGISwapChain* sc; ID3D12CommandQueue* queue; };
std::mutex g_created_mutex;
std::vector<CreatedQueue> g_created;
constexpr size_t kCreatedKept = 8;

bool queue_removed(ID3D12CommandQueue* q) {
    ID3D12Device* d = nullptr;
    const bool removed = SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) && d && d->GetDeviceRemovedReason() != S_OK;
    release(d);
    return removed;
}

ID3D12CommandQueue* created_queue(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> g(g_created_mutex);
    for (auto it = g_created.begin(); it != g_created.end(); ++it) {
        if (it->sc != sc) continue;
        if (queue_removed(it->queue)) { it->queue->Release(); g_created.erase(it); return nullptr; }
        it->queue->AddRef();
        return it->queue;
    }
    return nullptr;
}

std::atomic<ULONGLONG> g_system_drawn_ms{0};

void draw_on(IDXGISwapChain* sc, UINT flags, IUnknown* known = nullptr) {
    if (!known) ++g_presents;
    if (t_inside || (flags & DXGI_PRESENT_TEST)) return;
    t_inside = true;
    std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
    if (known) lock.lock();
    else (void)lock.try_lock();
    const auto now = std::chrono::steady_clock::now();
    const bool menu_here = lock.owns_lock();
    struct SameFrame { bool was; bool on; ~SameFrame() { if (on) t_same_frame = was; } } same{t_same_frame, false};
    if (!known && menu_here && GetTickCount64() - g_game_present_ms.load() < 1000) {
        same.on = true;
        t_same_frame = !g_game_frame_new.exchange(false);
    }
    struct Reap { std::chrono::steady_clock::time_point now; bool owned; ~Reap() {
        if (owned) { let_go_idle(g_d3d11, now); let_go_idle(g_d3d12, now); }
    } } reap{now, lock.owns_lock()};
    const auto said = [sc, known](const char* api) {
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        say(std::string("drawn over ") + (known ? "the game's own " : "a ") + api + " swap chain, " +
            std::to_string(desc.BufferDesc.Width) + "x" + std::to_string(desc.BufferDesc.Height) +
            ", DXGI format " + std::to_string(int(desc.BufferDesc.Format)) + ": " +
            encoding_of(sc, desc.BufferDesc.Format).name);
    };
    try {
        ID3D11Device* d11 = nullptr;
        ID3D12CommandQueue* q12 = nullptr;
        ID3D12Device* d12dev = nullptr;
        if (known) {
            if (FAILED(known->QueryInterface(IID_PPV_ARGS(&d11))) || !d11) {
                release(d11);
                if (FAILED(known->QueryInterface(IID_PPV_ARGS(&q12)))) q12 = nullptr;
            }
            if (!d11 && !q12) { t_inside = false; return; }
        } else if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d11))) || !d11) {
            release(d11);
            if ((FAILED(sc->GetDevice(IID_PPV_ARGS(&q12))) || !q12) && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12dev))) &&
                d12dev) {
                release(q12);
                if ((q12 = created_queue(sc))) {
                    ID3D12Device* owner = nullptr;
                    if (FAILED(q12->GetDevice(IID_PPV_ARGS(&owner))) || owner != d12dev) release(q12);
                    release(owner);
                }
                if (ID3D12CommandQueue* last = q12 ? nullptr : last_direct_queue()) {
                    ID3D12Device* owner = nullptr;
                    if (SUCCEEDED(last->GetDevice(IID_PPV_ARGS(&owner))) && owner == d12dev) { q12 = last; q12->AddRef(); }
                    release(owner);
                }
                release(d12dev);
                if (!q12) {
                    t_inside = false;
                    return;
                }
            }
        }
        static const bool d3d11_menu_enabled = [] {
            const char* e = std::getenv("OPENRR_D3D11_MENU");
            return e && e[0] == '1';
        }();
        if (d11 && !d3d11_menu_enabled) {
            d11->Release();
            t_inside = false;
            return;
        }
        if (d11 && !menu_here) {
            if (const D3D11PresentHook hook = g_d3d11_hook.load()) hook(sc, d11);
            d11->Release();
        } else if (q12 && !menu_here) {
            if (const D3D12PresentHook hook = g_d3d12_hook.load()) {
                DXGI_SWAP_CHAIN_DESC d{};
                if (SUCCEEDED(sc->GetDesc(&d))) hook(sc, q12, int(encoding_of(sc, d.BufferDesc.Format).mode));
            }
            if (const D3D12ShownHook shown = g_d3d12_shown.load()) shown(sc, q12);
            q12->Release();
        } else if (d11) {
            auto& k = g_d3d11[d11];
            k.used = now;
            if (!k.overlay) {
                try {
                    k.overlay = std::make_unique<D3D11Overlay>(d11);
                } catch (...) {
                    d11->Release();
                    g_d3d11.erase(d11);
                    throw;
                }
                said("D3D11");
            }
            if (const D3D11PresentHook hook = known ? nullptr : g_d3d11_hook.load()) hook(sc, d11);
            d11->Release();
            g_api_name = "D3D11";
            k.overlay->present(sc);
            if (!known) g_system_drawn_ms = GetTickCount64();
        } else if (q12) {
            auto& k = g_d3d12[q12];
            k.used = now;
            if (!k.overlay) {
                ID3D12Device* d12 = nullptr;
                try {
                    if (FAILED(q12->GetDevice(IID_PPV_ARGS(&d12))) || !d12)
                        throw std::runtime_error("the swap chain's queue gives no device");
                    k.overlay = std::make_unique<D3D12Overlay>(d12, q12);
                } catch (...) {
                    release(d12);
                    q12->Release();
                    g_d3d12.erase(q12);
                    throw;
                }
                d12->Release();
                said("D3D12");
            }
            {
                struct Shown { IDXGISwapChain* sc; ID3D12CommandQueue* q; bool on; ~Shown() {
                    if (on) try { if (const D3D12ShownHook shown = g_d3d12_shown.load()) shown(sc, q); } catch (...) {}
                    q->Release();
                } } shown{sc, q12, !known};
                if (const D3D12PresentHook hook = known ? nullptr : g_d3d12_hook.load()) {
                    DXGI_SWAP_CHAIN_DESC d{};
                    if (SUCCEEDED(sc->GetDesc(&d))) hook(sc, q12, int(encoding_of(sc, d.BufferDesc.Format).mode));
                }
                g_api_name = "D3D12";
                k.overlay->present(sc);
            }
            if (!known) g_system_drawn_ms = GetTickCount64();
        }
    } catch (const std::exception& e) {
        if (g_failed_for != sc) {
            g_failed_for = sc;
            say(std::string("the menu is not drawn on this swap chain: ") + e.what());
        }
    }
    t_inside = false;
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
PresentFn g_present[2]{};
Present1Fn g_present1[2]{};
SetColorSpace1Fn g_colour_space[2]{};
std::mutex g_hook_vtable_mutex;
std::vector<void*> g_hooked_fns;
int g_present_used = 0, g_present1_used = 0, g_cs_used = 0;

template <int N> HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* sc, UINT interval, UINT flags) {
    draw_on(sc, flags);
    return g_present[N](sc, interval, flags);
}
template <int N> HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* sc, UINT interval, UINT flags,
                                                         const DXGI_PRESENT_PARAMETERS* p) {
    draw_on(sc, flags);
    return g_present1[N](sc, interval, flags, p);
}
template <int N> HRESULT STDMETHODCALLTYPE colour_space_hook(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space) {
    const HRESULT hr = g_colour_space[N](sc, space);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        g_spaces[sc] = space;
        g_last_space = int(space);
    }
    return hr;
}

void hook_sc_methods(IDXGISwapChain* sc) {
    if (!sc) return;
    std::lock_guard<std::mutex> lock(g_hook_vtable_mutex);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;

    void** vt = *reinterpret_cast<void***>(sc);
    auto try_hook = [&](void* fn, void* detour0, void* detour1, void** real0, void** real1, int& used) {
        if (!fn || used >= 2) return;
        if (std::find(g_hooked_fns.begin(), g_hooked_fns.end(), fn) != g_hooked_fns.end()) return;
        void* detour = (used == 0) ? detour0 : detour1;
        void** real = (used == 0) ? real0 : real1;
        if (MH_CreateHook(fn, detour, real) == MH_OK && MH_EnableHook(fn) == MH_OK) {
            ++used;
            g_hooked_fns.push_back(fn);
        }
    };

    try_hook(vt[8], reinterpret_cast<void*>(&present_hook<0>), reinterpret_cast<void*>(&present_hook<1>),
             reinterpret_cast<void**>(&g_present[0]), reinterpret_cast<void**>(&g_present[1]), g_present_used);

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        void** vt1 = *reinterpret_cast<void***>(sc1);
        try_hook(vt1[22], reinterpret_cast<void*>(&present1_hook<0>), reinterpret_cast<void*>(&present1_hook<1>),
                 reinterpret_cast<void**>(&g_present1[0]), reinterpret_cast<void**>(&g_present1[1]), g_present1_used);
        sc1->Release();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        void** vt3 = *reinterpret_cast<void***>(sc3);
        try_hook(vt3[38], reinterpret_cast<void*>(&colour_space_hook<0>), reinterpret_cast<void*>(&colour_space_hook<1>),
                 reinterpret_cast<void**>(&g_colour_space[0]), reinterpret_cast<void**>(&g_colour_space[1]), g_cs_used);
        sc3->Release();
    }
    openrr::pe::control::start_input();
}

// In-process probe using system dxgi.dll + d3d11.dll (no external probe.exe needed).
void install_now() {
    openrr::pe::control::start_input();
    {
        std::lock_guard<std::mutex> lock(g_hook_vtable_mutex);
        if (g_present_used > 0) return;
    }
    wchar_t sys_dir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sys_dir, MAX_PATH)) return;
    const std::wstring dxgi_path = std::wstring(sys_dir) + L"\\dxgi.dll";
    const std::wstring d3d11_path = std::wstring(sys_dir) + L"\\d3d11.dll";
    HMODULE sys_dxgi = GetModuleHandleW(dxgi_path.c_str());
    if (!sys_dxgi) sys_dxgi = LoadLibraryW(dxgi_path.c_str());
    HMODULE sys_d3d11 = GetModuleHandleW(d3d11_path.c_str());
    if (!sys_d3d11) sys_d3d11 = LoadLibraryW(d3d11_path.c_str());
    if (!sys_dxgi || !sys_d3d11) return;

    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto create_fac = reinterpret_cast<CreateFactory1Fn>(reinterpret_cast<void*>(GetProcAddress(sys_dxgi, "CreateDXGIFactory1")));
    auto create_d11 = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(sys_d3d11, "D3D11CreateDevice")));
    if (!create_fac || !create_d11) return;

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"openrr_present_probe_wnd";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return;

    IDXGIFactory2* factory = nullptr;
    ID3D11Device* device = nullptr;
    IDXGISwapChain1* sc = nullptr;
    if (SUCCEEDED(create_fac(IID_PPV_ARGS(&factory))) && factory &&
        SUCCEEDED(create_d11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)) &&
        device) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64; sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(device, window, &sd, nullptr, nullptr, &sc)) && sc) {
            hook_sc_methods(sc);
            say("DXGI Present hooked via in-process system probe");
        }
    }
    release(sc);
    release(device);
    release(factory);
    DestroyWindow(window);
}

} // namespace

void hook_swapchain_vtable(IDXGISwapChain* sc) {
    hook_sc_methods(sc);
}

void abandon_at_exit() {
    for (auto& [k, v] : g_vulkan) (void)v.release();
    for (auto& [k, v] : g_d3d11) (void)v.overlay.release();
    for (auto& [k, v] : g_d3d12) (void)v.overlay.release();
}

std::mutex g_vulkan_keys_mutex;
std::vector<std::pair<VkDevice, VkSwapchainKHR>> g_vulkan_keys;

const VkPresentInfoKHR* vulkan_present(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                                       VkQueue queue, uint32_t family, VkSwapchainKHR sc, VkFormat format,
                                       VkColorSpaceKHR space, VkExtent2D extent, const std::vector<VkImage>& images,
                                       const VkPresentInfoKHR* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static thread_local VkPresentInfoKHR result;
    if (!info || info->swapchainCount != 1 || !info->pImageIndices || info->pImageIndices[0] >= images.size()) return nullptr;
    auto& o = g_vulkan[sc];
    bool submitted_frame = false;
    try {
        openrr::pe::control::start_input();
        if (!o) {
            {
                std::lock_guard<std::mutex> keys(g_vulkan_keys_mutex);
                g_vulkan_keys.emplace_back(device, sc);
            }
            o = std::make_unique<VulkanOverlay>();
            o->init(instance, physical, device, queue, family, format, space, extent, images);
            say("drawn over a Vulkan swap chain, " + std::to_string(extent.width) + "x" + std::to_string(extent.height) +
                ": " + o->encoding.name);
        }
        if (o->failed || !o->input()) return nullptr;
        if (o->last && vkWaitForFences(device, 1, &o->last, VK_TRUE, 2000000000ull) != VK_SUCCESS) return nullptr;
        const uint32_t index = info->pImageIndices[0];
        auto& f = o->submits[index];
        if (vkWaitForFences(device, 1, &f.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return nullptr;
        vk_check(vkResetCommandBuffer(f.cmd, 0), "menu reset");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(f.cmd, &bi), "menu begin");
        g_api_name = "Vulkan";
        o->record(f.cmd, index);
        ++g_drawn;
        vk_check(vkEndCommandBuffer(f.cmd), "menu end");
        std::vector<VkPipelineStageFlags> waits(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = info->waitSemaphoreCount;
        si.pWaitSemaphores = info->pWaitSemaphores;
        si.pWaitDstStageMask = waits.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f.done;
        vk_check(vkResetFences(device, 1, &f.fence), "menu fence reset");
        const bool held = openrr::pe::vkdevice::lock_queue_held(queue);
        const VkResult submitted = vkQueueSubmit(queue, 1, &si, f.fence);
        openrr::pe::vkdevice::unlock_queue_held(queue, held);
        vk_check(submitted, "menu submit");
        o->last = f.fence;
        result = *info;
        result.waitSemaphoreCount = 1;
        result.pWaitSemaphores = &f.done;
        submitted_frame = true;
        return &result;
    } catch (const std::exception& e) {
        if (o) o->failed = true;
        say(std::string("Vulkan menu: ") + e.what());
        return submitted_frame ? &result : nullptr;
    }
}

void vulkan_forget(VkDevice device, VkSwapchainKHR sc) {
    {
        std::lock_guard<std::mutex> keys(g_vulkan_keys_mutex);
        const auto mine = [&](const std::pair<VkDevice, VkSwapchainKHR>& k) { return k.first == device && (!sc || k.second == sc); };
        if (std::none_of(g_vulkan_keys.begin(), g_vulkan_keys.end(), mine)) return;
        g_vulkan_keys.erase(std::remove_if(g_vulkan_keys.begin(), g_vulkan_keys.end(), mine), g_vulkan_keys.end());
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_vulkan.begin(); it != g_vulkan.end();) {
        auto& o = it->second;
        if (!o || (o->device == device && (!sc || it->first == sc))) {
            if (o) {
                const bool held = openrr::pe::vkdevice::lock_queue_held(o->queue);
                const VkResult idle = vkQueueWaitIdle(o->queue);
                openrr::pe::vkdevice::unlock_queue_held(o->queue, held);
                if (idle != VK_SUCCESS) (void)o.release();
            }
            it = g_vulkan.erase(it);
        } else ++it;
    }
}

void watch_queues(ID3D12Device* device) {
    static std::once_flag once;
    std::call_once(once, [device] {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* q = nullptr;
        if (!device || FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) return;
        void* fn = (*reinterpret_cast<void***>(q))[10];
        q->Release();
        const MH_STATUS init = MH_Initialize();
        if ((init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED)) {
            if (MH_CreateHook(fn, reinterpret_cast<void*>(&execute_hook), reinterpret_cast<void**>(&g_execute)) == MH_OK)
                MH_EnableHook(fn);
        }
    });
}

void on_d3d12_present(D3D12PresentHook hook) { g_d3d12_hook.store(hook); }
void on_d3d12_shown(D3D12ShownHook hook) { g_d3d12_shown.store(hook); }

void note_swapchain_queue(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue) {
    if (!swap_chain || !queue) return;
    {
        ID3D12CommandQueue* own = nullptr;
        const bool handed = SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&own))) && own;
        release(own);
        if (handed) return;
    }
    std::lock_guard<std::mutex> g(g_created_mutex);
    for (auto it = g_created.begin(); it != g_created.end();)
        if (it->sc == swap_chain || queue_removed(it->queue)) { it->queue->Release(); it = g_created.erase(it); }
        else ++it;
    while (g_created.size() >= kCreatedKept) { g_created.front().queue->Release(); g_created.erase(g_created.begin()); }
    queue->AddRef();
    g_created.push_back({swap_chain, queue});
}

void draw_game(IDXGISwapChain* swap_chain, IUnknown* device, bool new_frame) {
    if (!swap_chain || !device) return;
    const ULONGLONG now = GetTickCount64();
    g_game_present_ms = now;
    if (now - g_system_drawn_ms.load() < 1000) {
        if (new_frame) g_game_frame_new = true;
        return;
    }
    g_game_frame_new = false;
    t_same_frame = !new_frame;
    draw_on(swap_chain, 0, device);
    t_same_frame = false;
}

void on_d3d11_present(D3D11PresentHook hook) { g_d3d11_hook.store(hook); }
void on_execute(ExecuteObserver observer) { g_execute_observer.store(observer); }

bool colour_space_of(IDXGISwapChain* swap_chain, int* space) {
    std::lock_guard<std::mutex> lock(g_spaces_mutex);
    const auto it = g_spaces.find(swap_chain);
    if (it == g_spaces.end()) return false;
    *space = int(it->second);
    return true;
}

void install() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            try {
                install_now();
            } catch (const std::exception& e) {
                say(std::string("no menu: ") + e.what());
            }
        }).detach();
    });
}

} // namespace openrr::pe::menu
