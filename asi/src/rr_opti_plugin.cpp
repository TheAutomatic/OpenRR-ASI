#include "rr_opti_plugin.hpp"

#include "rr_data_paths.hpp"
#include "rr_ini_edit.hpp"
#include "rr_lang.hpp"
#include "rr_number.hpp"
#include "rr_path_win.hpp"
#include "rr_pe_config.hpp"
#include "rr_pe_control.hpp"
#include "rr_pe_log.hpp"
#include "opennr_rr_api.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef OPENRR_BUILD_STAMP
#define OPENRR_BUILD_STAMP "dev"
#endif

namespace openrr::pe::opti_plugin {
namespace {

const OptiPluginApi* g_api = nullptr;
OptiPlugin* g_plugin = nullptr;
DeferredInitFn g_deferred_init = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_provider_registered{false};
std::atomic<bool> g_disabled_at_launch{false};
std::atomic<bool> g_missing_provider_api{false};

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool bool_val(const std::string& s, bool def) {
    const std::string l = lower(trim(s));
    if (l == "1" || l == "true" || l == "on" || l == "yes") return true;
    if (l == "0" || l == "false" || l == "off" || l == "no") return false;
    return def;
}

bool is_existing_file(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attrs = openrr::u8::attributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_abs_path(const std::string& p) {
    return (p.size() >= 2 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
}

std::string module_dir() {
    return std::string(openrr::pe::module_folder());
}

std::string lang_folder() {
    const std::string base = module_dir();
    if (!base.empty()) {
        const auto found = openrr::pe::installed_data_dir(openrr::u8::path(base), L"lang");
        if (!found.empty()) return openrr::u8::text(found / L"lang");
    }
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (openrr::u8::attributes_ex(base + "\\openrr-amd\\lang", &a) &&
        (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return base + "\\openrr-amd\\lang";
    return base + "\\lang";
}

std::string optiscaler_ini_path() {
    const std::string base = module_dir();
    return (base.empty() ? std::string() : base + "\\") + "OptiScaler.ini";
}

bool optiscaler_has_custom_ttf() {
    const std::string ini_path = optiscaler_ini_path();
    char hq_buf[32]{};
    openrr::u8::profile("Menu", "UseHQFont", "auto", hq_buf, sizeof(hq_buf), ini_path);
    const std::string hq = lower(trim(hq_buf));
    if (hq == "false" || hq == "0" || hq == "off" || hq == "no") return false;

    char font_buf[1024]{};
    openrr::u8::profile("Menu", "TTFFontPath", "auto", font_buf, sizeof(font_buf), ini_path);
    const std::string raw = trim(font_buf);
    if (raw.empty() || lower(raw) == "auto") return false;
    // OptiScaler passes TTFFontPath straight to ImFontAtlas::AddFontFromFileTTF (absolute or CWD-relative).
    return is_existing_file(raw);
}

bool table_needs_non_latin_font() {
    const auto& cps = openrr::lang::table().code_points;
    return std::any_of(cps.begin(), cps.end(), [](unsigned cp) { return cp > 0x00FFu; });
}

std::string pick_cjk_font_path() {
    const std::string folder = lang_folder();
    std::vector<std::string> candidates;
    const std::string& named = openrr::lang::table().font;
    if (!named.empty()) {
        candidates.push_back(is_abs_path(named) ? named : folder + "\\" + named);
    }
    candidates.push_back(folder + "\\fonts\\NotoSansCJKsc-Regular.ttf");
    candidates.push_back("C:\\Windows\\Fonts\\simhei.ttf");
    candidates.push_back("C:\\Windows\\Fonts\\msyh.ttc");
    candidates.push_back("C:\\Windows\\Fonts\\YuGothR.ttc");
    candidates.push_back("C:\\Windows\\Fonts\\meiryo.ttc");
    candidates.push_back("C:\\Windows\\Fonts\\malgun.ttf");
    for (std::string& c : candidates) {
        std::replace(c.begin(), c.end(), '/', '\\');
        if (is_existing_file(c)) return c;
    }
    return {};
}

struct IniStamp {
    uint64_t mtime = 0;
    uint64_t size = 0;
    bool valid() const { return mtime != 0; }
    bool operator==(const IniStamp& o) const { return mtime == o.mtime && size == o.size; }
    bool operator!=(const IniStamp& o) const { return !(*this == o); }
};

IniStamp ini_stamp(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!openrr::u8::attributes_ex(path, &d)) return {};
    return {
        (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime,
        (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow,
    };
}

enum class FontRepairResult { Unreadable, CustomFont, Ok, Repaired };

FontRepairResult repair_cjk_font_ini(const std::string& ini_path, const std::string& font_path,
                                     bool allow_create, bool force_font_path, IniStamp* out_stamp) {
    openrr::ini_edit::Ini opti_ini{openrr::u8::path(ini_path)};
    if (!opti_ini.refresh()) {
        const DWORD attrs = openrr::u8::attributes(ini_path);
        const DWORD err = GetLastError();
        if (!allow_create || attrs != INVALID_FILE_ATTRIBUTES || err != ERROR_FILE_NOT_FOUND) {
            return FontRepairResult::Unreadable;
        }
    }

    auto strip_ini_val = [](std::string v) {
        v = trim(v);
        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
            const size_t end = v.find(v.front(), 1);
            return end != std::string::npos ? v.substr(1, end - 1) : v.substr(1);
        }
        const size_t c = v.find_first_of(";#");
        if (c != std::string::npos) v.resize(c);
        return trim(v);
    };

    const std::string hq = lower(strip_ini_val(opti_ini.get("Menu", "UseHQFont", "auto")));
    const bool hq_disabled = (hq == "false" || hq == "0" || hq == "off" || hq == "no");
    const std::string cur_font = strip_ini_val(opti_ini.get("Menu", "TTFFontPath", "auto"));
    const bool font_auto = cur_font.empty() || lower(cur_font) == "auto";

    if (!force_font_path && !font_auto) {
        if (lower(cur_font) != lower(font_path)) return FontRepairResult::CustomFont;
        if (!hq_disabled) return FontRepairResult::Ok;
    }

    opti_ini.set("Menu", "UseHQFont", "true");
    if (force_font_path || font_auto) {
        opti_ini.set("Menu", "TTFFontPath", font_path);
    }
    if (!opti_ini.save(/*notify_config=*/false)) return FontRepairResult::Unreadable;
    if (out_stamp) *out_stamp = ini_stamp(ini_path);
    return FontRepairResult::Repaired;
}

struct PendingFontGuard {
    std::mutex mutex;
    std::atomic<bool> active{false};
    bool watcher_running = false; // guarded by mutex
    std::string ini_path;
    std::string font_path;
    IniStamp last_known_stamp{};
    IniStamp pending_stamp{};
    ULONGLONG change_seen_ms = 0;
    ULONGLONG last_check_ms = 0;
    ULONGLONG last_write_ms = 0;
};

// Immortal singleton allocated lazily only when armed; never constructed or destroyed during DllMain/ExitProcess.
std::atomic<PendingFontGuard*> g_pending_font_ptr{nullptr};

void poll_pending_font();

DWORD WINAPI pending_font_watcher(void* param) {
    HMODULE self = static_cast<HMODULE>(param);
    PendingFontGuard* g = g_pending_font_ptr.load(std::memory_order_acquire);
    while (g) {
        Sleep(500);
        if (g->active.load(std::memory_order_acquire)) {
            poll_pending_font();
            continue;
        }
        std::lock_guard<std::mutex> lock(g->mutex);
        if (!g->active.load(std::memory_order_relaxed)) {
            g->watcher_running = false;
            break;
        }
    }
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

void arm_pending_font(const std::string& ini_path, const std::string& font_path, IniStamp stamp) {
    PendingFontGuard* g = g_pending_font_ptr.load(std::memory_order_acquire);
    if (!g) {
        auto* fresh = new PendingFontGuard();
        if (g_pending_font_ptr.compare_exchange_strong(g, fresh, std::memory_order_acq_rel, std::memory_order_acquire)) {
            g = fresh;
        } else {
            delete fresh;
        }
    }
    bool spawn_watcher = false;
    {
        std::lock_guard<std::mutex> lock(g->mutex);
        g->ini_path = ini_path;
        g->font_path = font_path;
        g->last_known_stamp = stamp;
        g->pending_stamp = {};
        g->change_seen_ms = 0;
        g->last_check_ms = GetTickCount64();
        g->last_write_ms = g->last_check_ms;
        g->active.store(true, std::memory_order_release);
        if (!g->watcher_running) {
            g->watcher_running = true;
            spawn_watcher = true;
        }
    }
    if (spawn_watcher) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&pending_font_watcher), &self);
        HANDLE t = CreateThread(nullptr, 0, &pending_font_watcher, self, 0, nullptr);
        if (t) {
            CloseHandle(t);
        } else {
            std::lock_guard<std::mutex> lock(g->mutex);
            g->watcher_running = false;
            if (self) FreeLibrary(self);
        }
    }
}

void poll_pending_font() {
    PendingFontGuard* g = g_pending_font_ptr.load(std::memory_order_acquire);
    if (!g || !g->active.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lock(g->mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g->active.load(std::memory_order_relaxed)) return;

    const ULONGLONG now = GetTickCount64();
    if (now >= g->last_check_ms && (now - g->last_check_ms) < 500) return;
    g->last_check_ms = now;

    const IniStamp cur_stamp = ini_stamp(g->ini_path);
    if (!cur_stamp.valid() || cur_stamp == g->last_known_stamp) {
        g->pending_stamp = {};
        g->change_seen_ms = 0;
        return;
    }
    if (cur_stamp != g->pending_stamp) {
        g->pending_stamp = cur_stamp;
        g->change_seen_ms = now;
        return;
    }
    // Wait until OptiScaler.ini has been stable for 1.5s and at least 2.0s passed since our last write.
    if (now < g->change_seen_ms || (now - g->change_seen_ms) < 1500) return;
    if (now < g->last_write_ms || (now - g->last_write_ms) < 2000) return;

    IniStamp new_stamp{};
    const FontRepairResult r = repair_cjk_font_ini(g->ini_path, g->font_path, /*allow_create=*/false,
                                                   /*force_font_path=*/false, &new_stamp);
    if (r == FontRepairResult::Repaired) {
        g->last_known_stamp = new_stamp;
        g->pending_stamp = {};
        g->change_seen_ms = 0;
        g->last_write_ms = now;
        openrr::pe::log("[openrr] restored OptiScaler.ini [Menu] TTFFontPath after external save");
    } else if (r == FontRepairResult::CustomFont) {
        // User manually configured a different custom TTF font; respect their edit and stop guarding.
        g->last_known_stamp = cur_stamp;
        g->pending_stamp = {};
        g->change_seen_ms = 0;
        g->active.store(false, std::memory_order_release);
    } else if (r == FontRepairResult::Ok) {
        g->last_known_stamp = cur_stamp;
        g->pending_stamp = {};
        g->change_seen_ms = 0;
    } else {
        g->last_write_ms = now;
    }
}

const char* tr_id(const char* text, const char* id_suffix) {
    thread_local char buf[512];
    std::snprintf(buf, sizeof(buf), "%s%s", text ? text : "", id_suffix ? id_suffix : "");
    return buf;
}

std::string fmt(const char* pattern, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, pattern);
    std::vsnprintf(buf, sizeof(buf), pattern, ap);
    va_end(ap);
    return std::string(buf);
}

struct TabState {
    openrr::ini_edit::Ini ini{openrr::u8::path(module_dir()) / L"openrr-amd.ini"};
    bool opti_font_ready_at_launch = false;
    bool font_checked = false;
    bool font_configured_pending_restart = false;
    std::string font_error;
    float dm_scale_ini = -1.0f;
    float dm_scale_edit = 1.0f;
    ULONGLONG last_draw_ms = 0;
    bool reinit_requested_notice = false;
    ULONGLONG reinit_notice_until = 0;
    std::vector<std::pair<std::string, std::string>> cached_langs;
    ULONGLONG langs_checked_ms = 0;

    void refresh_langs() {
        const ULONGLONG now = GetTickCount64();
        if (!cached_langs.empty() && langs_checked_ms != 0 && (now - langs_checked_ms) < 1500) return;
        langs_checked_ms = now;
        cached_langs = openrr::lang::list(lang_folder());
    }

    void sync_language() {
        if (!font_checked) {
            opti_font_ready_at_launch = optiscaler_has_custom_ttf();
            font_checked = true;
        }
        (void)openrr::lang::sync(lang_folder(), ini.get("OpenRR", "Language", ""));
        if (table_needs_non_latin_font() && !opti_font_ready_at_launch) {
            openrr::lang::suspended() = true;
        } else {
            openrr::lang::suspended() = false;
        }
    }

    bool checkbox_ini(const char* label, const char* id_suffix, const char* key, bool def) {
        bool v = bool_val(ini.get("OpenRR", key, def ? "true" : "false"), def);
        if (g_api->Checkbox(tr_id(label, id_suffix), &v)) {
            ini.set("OpenRR", key, v ? "true" : "false");
            return true;
        }
        return false;
    }

    void draw() {
        using openrr::lang::tr;

        const ULONGLONG now_draw = GetTickCount64();
        const bool tab_reopened = (last_draw_ms == 0) || (now_draw >= last_draw_ms && (now_draw - last_draw_ms) > 250);
        last_draw_ms = now_draw;

        ini.refresh();
        sync_language();

        const openrr::control::Shared st = openrr::pe::control::snapshot();
        const char* api_name = st.route == 1 ? "D3D12" : st.route == 2 ? "Vulkan" : "Standby";
        const char* build_str = st.asi_build[0] ? st.asi_build : OPENRR_BUILD_STAMP;

        // ---- Status card ----
        g_api->Section(tr("Status"));
        g_api->TextDisabled(fmt("OpenRR %s (%s)  |  %s",
                                ONR_RR_RUNTIME_VERSION, build_str, api_name)
                                .c_str());

        if (g_missing_provider_api.load(std::memory_order_acquire)) {
            g_api->TextColored(1.0f, 0.45f, 0.40f, 1.0f,
                               tr("OptiScaler Plugin API v3 (RegisterProvider) is unavailable. Please update to OptiScaler v0.10 nightly."));
        } else if (g_disabled_at_launch.load(std::memory_order_acquire)) {
            g_api->TextColored(0.95f, 0.78f, 0.25f, 1.0f,
                               tr("OpenRR provider registration was disabled at startup ([OpenRR] Enabled = false). Enable it below and restart the game."));
        } else if (!g_provider_registered.load(std::memory_order_acquire)) {
            g_api->TextColored(1.0f, 0.45f, 0.40f, 1.0f,
                               tr("OpenRR could not register as an OptiScaler DLSSD provider. Check openrr-amd.log."));
        } else if (!st.rr_active) {
            g_api->TextDisabled(tr("Idle: waiting for the game to create a DLSS Ray Reconstruction feature."));
            if (st.message[0]) {
                g_api->TextDisabled(fmt(tr("Note: %s"), st.message).c_str());
            }
        } else {
            const char* st_name = st.state == ONR_RR_STATE_READY    ? tr("Ready")
                                : st.state == ONR_RR_STATE_BUILDING ? tr("Building network...")
                                : st.state == ONR_RR_STATE_FAILED   ? tr("Failed")
                                                                    : tr("Unknown");
            const bool show_msg = st.message[0] &&
                                  !(st.state == ONR_RR_STATE_READY && lower(trim(st.message)) == "ready");
            const std::string state_line = fmt(tr("State: %s%s%s%s"), st_name,
                                               show_msg ? " (" : "", show_msg ? st.message : "", show_msg ? ")" : "");
            if (st.state == ONR_RR_STATE_READY) {
                g_api->TextColored(0.45f, 0.85f, 0.45f, 1.0f, state_line.c_str());
            } else if (st.state == ONR_RR_STATE_BUILDING) {
                g_api->TextColored(0.95f, 0.78f, 0.25f, 1.0f, state_line.c_str());
            } else if (st.state == ONR_RR_STATE_FAILED) {
                g_api->TextColored(1.0f, 0.45f, 0.40f, 1.0f, state_line.c_str());
            } else {
                g_api->Text(state_line.c_str());
            }

            const char* be_name = st.backend == ONR_RR_BACKEND_D3D12       ? tr("D3D12 Direct")
                                : st.backend == ONR_RR_BACKEND_VULKAN_HOST ? tr("Vulkan Native")
                                                                           : tr("Vulkan Bridge");
            g_api->Text(fmt(tr("Backend: %s"), be_name).c_str());
            if (st.backend_message[0]) {
                g_api->TextDisabled(fmt(tr("Backend note: %s"), st.backend_message).c_str());
            }
            if (st.render_width && st.output_width) {
                g_api->Text(fmt(tr("Resolution: %ux%u -> %ux%u"),
                                st.render_width, st.render_height, st.output_width, st.output_height)
                                .c_str());
            }
            g_api->Text(fmt(tr("GPU time (raw): %.2f ms (p50 %.2f ms, p95 %.2f ms)  |  Frames: %u"),
                            double(st.gpu_raw_last), double(st.gpu_raw_p50), double(st.gpu_raw_p95), st.frames)
                            .c_str());
            g_api->TextDisabled(fmt(tr("Est. w/o async preemption: %.2f ms (avg %.2f ms)"),
                                    double(st.gpu_ms_last), double(st.gpu_ms_average))
                                    .c_str());
            if (st.weights_source[0]) {
                g_api->TextDisabled(fmt(tr("Weights: %s"), st.weights_source).c_str());
            }
        }
        if (st.coexist_mochi) {
            g_api->TextDisabled(tr("Coexistence: dlssnr-amd.asi detected (Direct path active)"));
        }

        // ---- Execution & Backend card ----
        g_api->Section(tr("Execution & Backend"));
        checkbox_ini(tr("Enable OpenRR (Ray Reconstruction)"), "##rr_enabled", "Enabled", true);
        g_api->ItemTooltip(tr("Registers OpenRR as an OptiScaler Ray Reconstruction provider (restart required when toggling). Use OptiScaler's Upscalers -> RR Upscaler selector to switch providers live."));

        const std::string be = lower(trim(ini.get("OpenRR", "Backend", "auto")));
        int32_t be_idx = (be == "d3d12") ? 1 : (be == "vulkan") ? 2 : 0;
        const char* be_items[] = {
            tr("Auto (D3D12 Direct preferred)"),
            tr("D3D12 Direct"),
            tr("Vulkan Bridge"),
        };
        if (g_api->Combo(tr_id(tr("D3D12 Backend"), "##rr_backend"), &be_idx, be_items, 3, nullptr)) {
            ini.set("OpenRR", "Backend", be_idx == 1 ? "d3d12" : be_idx == 2 ? "vulkan" : "auto");
        }

        int32_t mode_idx = openrr::pe::network_mode_level(ini.get("OpenRR", "Mode", "new"));
        const char* mode_values[] = {"legacy", "new", "fast"};
        const char* mode_items[] = {
            tr("Legacy"),
            tr("New (recommended)"),
            tr("Fast (experimental)"),
        };
        if (g_api->Combo(tr_id(tr("Network Mode"), "##rr_mode"), &mode_idx, mode_items, 3, nullptr)) {
            ini.set("OpenRR", "Mode", mode_values[std::clamp(mode_idx, int32_t(0), int32_t(2))]);
        }
        if (st.rr_active) {
            const char* active_modes[] = {
                tr("Legacy"),
                tr("New"),
                tr("Building fast kernels"),
                tr("Fast kernels unavailable"),
                tr("Fast (experimental)"),
            };
            if (st.fast_mode < (sizeof(active_modes) / sizeof(active_modes[0]))) {
                g_api->TextDisabled(fmt(tr("Runtime: %s"), active_modes[st.fast_mode]).c_str());
            }
        }

        const std::string cs = lower(trim(ini.get("OpenRR", "CoexistSplit", "auto")));
        int32_t cs_idx = (cs == "off" || cs == "0" || cs == "false") ? 1 : 0;
        const char* cs_items[] = {
            tr("Auto (disable split when dlssnr-amd is present)"),
            tr("Off (never split command lists)"),
        };
        if (g_api->Combo(tr_id(tr("Coexist Split"), "##rr_coexist"), &cs_idx, cs_items, 2, nullptr)) {
            ini.set("OpenRR", "CoexistSplit", cs_idx == 1 ? "off" : "auto");
        }

        const std::string hw = lower(trim(ini.get("OpenRR", "HardwareDepth", "auto")));
        int32_t hw_idx = (hw == "true" || hw == "1" || hw == "on" || hw == "yes") ? 1
                       : (hw == "false" || hw == "0" || hw == "off" || hw == "no") ? 2
                                                                                   : 0;
        const char* hw_items[] = {
            tr("Auto"),
            tr("Hardware Depth (NDC)"),
            tr("Linear Depth"),
        };
        if (g_api->Combo(tr_id(tr("Depth Type"), "##rr_depth"), &hw_idx, hw_items, 3, nullptr)) {
            ini.set("OpenRR", "HardwareDepth", hw_idx == 1 ? "true" : hw_idx == 2 ? "false" : "auto");
        }

        const std::string vkl = lower(trim(ini.get("OpenRR", "VkInputLayout", "shader_read")));
        int32_t vkl_idx = (vkl == "general") ? 1 : 0;
        const char* vkl_items[] = {
            tr("Shader Read Optimal"),
            tr("General"),
        };
        if (g_api->Combo(tr_id(tr("Vulkan Input Layout"), "##rr_vk_layout"), &vkl_idx, vkl_items, 2, nullptr)) {
            ini.set("OpenRR", "VkInputLayout", vkl_idx == 1 ? "general" : "shader_read");
        }

        if (OPTI_API_HAS(g_api, ReInitUpscalers)) {
            g_api->Spacing();
            if (g_api->Button(tr_id(tr("Re-init Upscaler"), "##rr_reinit"))) {
                ini.save();
                if (g_api->ReInitUpscalers(g_plugin)) {
                    reinit_requested_notice = true;
                    reinit_notice_until = GetTickCount64() + 3000;
                }
            }
            g_api->ItemTooltip(tr("Recreates active upscalers so backend or depth changes take effect without restarting."));
            if (reinit_requested_notice && GetTickCount64() < reinit_notice_until) {
                g_api->SameLine();
                g_api->TextColored(0.45f, 0.85f, 0.45f, 1.0f, tr("Re-init requested"));
            }
        }

        // ---- Guides card ----
        g_api->Section(tr("Guides"));
        checkbox_ini(tr("Use Disocclusion Mask"), "##rr_dm_use", "UseDisocclusionMask", true);
        checkbox_ini(tr("Invert Disocclusion Mask"), "##rr_dm_inv", "InvertDisocclusionMask", false);
        checkbox_ini(tr("Clamp Disocclusion Mask (0..1)"), "##rr_dm_clamp", "ClampDisocclusionMask", true);

        const float ini_scale = std::clamp(openrr::parse_float(ini.get("OpenRR", "DisocclusionMaskScale", "1.0"), 1.0f), 0.1f, 4.0f);
        if (tab_reopened || ini_scale != dm_scale_ini) {
            dm_scale_ini = ini_scale;
            dm_scale_edit = ini_scale;
        }
        (void)g_api->SliderFloat(tr_id(tr("Disocclusion Mask Scale"), "##rr_dm_scale"), &dm_scale_edit, 0.1f, 4.0f, "%.2f");
        if (g_api->ItemDeactivatedAfterEdit()) {
            dm_scale_edit = std::clamp(dm_scale_edit, 0.1f, 4.0f);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", double(dm_scale_edit));
            ini.set("OpenRR", "DisocclusionMaskScale", buf);
            dm_scale_ini = dm_scale_edit;
        }

        g_api->Separator();
        checkbox_ini(tr("Use SSS Guide (ColorBeforeFog)"), "##rr_sss", "UseSssGuide", true);
        checkbox_ini(tr("Use Responsivity Mask"), "##rr_resp", "UseResponsivityMask", false);
        checkbox_ini(tr("Use Color Before Transparency"), "##rr_cbt", "UseColorBeforeTransparency", false);
        checkbox_ini(tr("Use Depth of Field Guide"), "##rr_dof", "UseDepthOfFieldGuide", false);
        checkbox_ini(tr("Use Reflection Motion Vectors"), "##rr_smv", "UseReflectionMotionVectors", false);
        checkbox_ini(tr("Use Transparency Layer Opacity & Motion"), "##rr_tl", "UseLayerOpacityAndMotion", false);

        // ---- Language card ----
        g_api->Section(tr("Language"));
        refresh_langs();
        std::vector<const char*> lang_items;
        lang_items.reserve(cached_langs.size());
        int32_t lang_sel = 0;
        const std::string lang_cur = lower(trim(ini.get("OpenRR", "Language", "")));
        for (const auto& [stem, shown] : cached_langs) {
            // While OptiScaler lacks a CJK font, show ASCII stem for non-ASCII language labels so the dropdown stays readable.
            const bool has_non_ascii = std::any_of(shown.begin(), shown.end(),
                                                   [](unsigned char ch) { return ch >= 0x80; });
            lang_items.push_back((!opti_font_ready_at_launch && has_non_ascii && !stem.empty())
                                     ? stem.c_str()
                                     : shown.c_str());
            if (lower(stem) == lang_cur) lang_sel = int32_t(lang_items.size()) - 1;
        }
        if (!lang_items.empty() &&
            g_api->Combo(tr_id(tr("Language"), "##rr_lang"), &lang_sel, lang_items.data(), int32_t(lang_items.size()), nullptr)) {
            if (lang_sel >= 0 && size_t(lang_sel) < cached_langs.size()) {
                ini.set("OpenRR", "Language", cached_langs[size_t(lang_sel)].first);
                openrr::pe::log("[openrr] Language -> %s", cached_langs[size_t(lang_sel)].first.c_str());
            }
        }
        g_api->ItemTooltip(tr("openrr-amd\\lang\\<name>.txt; the file reloads when it changes."));

        if (table_needs_non_latin_font() && !opti_font_ready_at_launch) {
            if (font_configured_pending_restart) {
                g_api->TextColored(0.45f, 0.85f, 0.45f, 1.0f,
                                   tr("Updated OptiScaler.ini ([Menu] TTFFontPath). Restart the game to display non-Latin text."));
            } else {
                g_api->TextColored(0.95f, 0.78f, 0.25f, 1.0f,
                                   tr("OptiScaler's built-in font has Latin glyphs only. Configure [Menu] TTFFontPath in OptiScaler.ini and restart to display this language."));
                if (g_api->Button(tr_id(tr("Configure CJK Font in OptiScaler.ini"), "##rr_cfg_font"))) {
                    const std::string font_path = pick_cjk_font_path();
                    if (font_path.empty()) {
                        font_error = tr("No CJK font file found in openrr-amd\\lang\\fonts or C:\\Windows\\Fonts.");
                    } else {
                        const std::string opti_path = optiscaler_ini_path();
                        IniStamp stamp{};
                        if (repair_cjk_font_ini(opti_path, font_path, /*allow_create=*/true,
                                                /*force_font_path=*/true, &stamp) == FontRepairResult::Repaired) {
                            arm_pending_font(opti_path, font_path, stamp);
                            font_configured_pending_restart = true;
                            font_error.clear();
                            openrr::pe::log("[openrr] configured OptiScaler.ini [Menu] TTFFontPath = %s", font_path.c_str());
                        } else {
                            font_error = tr("Could not write OptiScaler.ini. Check file permissions.");
                            openrr::pe::log("[openrr] failed to write OptiScaler.ini for CJK font");
                        }
                    }
                }
                if (!font_error.empty()) {
                    g_api->TextColored(1.0f, 0.45f, 0.40f, 1.0f, font_error.c_str());
                }
            }
        }

        ini.save();
    }
};

void draw_tab(void*) {
    try {
        if (!g_api) return;
        if (g_deferred_init) g_deferred_init();
        poll_pending_font();
        static TabState state;
        state.draw();
    } catch (...) {
    }
}

bool has_required_tab_api(const OptiPluginApi* api) {
    return api &&
           OPTI_API_HAS(api, RegisterTab) &&
           OPTI_API_HAS(api, Section) &&
           OPTI_API_HAS(api, Separator) &&
           OPTI_API_HAS(api, SameLine) &&
           OPTI_API_HAS(api, Spacing) &&
           OPTI_API_HAS(api, ItemTooltip) &&
           OPTI_API_HAS(api, ItemDeactivatedAfterEdit) &&
           OPTI_API_HAS(api, Text) &&
           OPTI_API_HAS(api, TextDisabled) &&
           OPTI_API_HAS(api, TextColored) &&
           OPTI_API_HAS(api, Button) &&
           OPTI_API_HAS(api, Checkbox) &&
           OPTI_API_HAS(api, SliderFloat) &&
           OPTI_API_HAS(api, Combo);
}

} // namespace

bool init(const OptiPluginApi* api, OptiPlugin* plugin, OptiGetProcFn provider_get_proc,
          DeferredInitFn deferred_init) {
    try {
        if (!plugin || !has_required_tab_api(api)) return false;
        if (g_active.exchange(true, std::memory_order_acq_rel)) return true;
        g_api = api;
        g_plugin = plugin;
        g_deferred_init = deferred_init;

        const bool tab_ok = api->RegisterTab(plugin, "OpenRR", "OpenRR Ray Reconstruction for AMD GPUs", &draw_tab, nullptr);
        if (!tab_ok) {
            g_active.store(false, std::memory_order_release);
            openrr::pe::log("[openrr] OptiScaler RegisterTab failed");
            return false;
        }

        const openrr::pe::Config cfg = openrr::pe::current_config();
        if (!cfg.enabled) {
            g_disabled_at_launch.store(true, std::memory_order_release);
            openrr::pe::log("[openrr] OptiPluginInit: [OpenRR] Enabled = false; skipping OptiProvider_Dlssd registration");
            if (OPTI_API_HAS(api, Log)) {
                api->Log(plugin, OptiLog_Info, "OpenRR tab registered; DLSSD provider disabled by openrr-amd.ini (Enabled = false)");
            }
            return true;
        }

        if (!OPTI_API_HAS(api, RegisterProvider) || !provider_get_proc) {
            g_missing_provider_api.store(true, std::memory_order_release);
            openrr::pe::log("[openrr] OptiPluginInit: OptiScaler Plugin API v%u lacks RegisterProvider (v3 required)",
                            unsigned(api->version));
            if (OPTI_API_HAS(api, Log)) {
                api->Log(plugin, OptiLog_Warn, "OptiScaler Plugin API v3 (RegisterProvider) is required for OpenRR");
            }
            return true;
        }

        const bool reg_ok = api->RegisterProvider(plugin, "OpenRR", OptiProvider_Dlssd, provider_get_proc, nullptr);
        g_provider_registered.store(reg_ok, std::memory_order_release);
        openrr::pe::log("[openrr] OptiPluginInit: API v%u, tab=ok, OptiProvider_Dlssd(\"OpenRR\")=%s",
                        unsigned(api->version), reg_ok ? "ok" : "failed");
        if (OPTI_API_HAS(api, Log)) {
            api->Log(plugin, reg_ok ? OptiLog_Info : OptiLog_Error,
                     reg_ok ? "Registered OpenRR tab and DLSSD provider" : "Failed to register OpenRR DLSSD provider");
        }
        return true;
    } catch (...) {
        return false;
    }
}

void flush_pending_font() {
    try {
        PendingFontGuard* g = g_pending_font_ptr.load(std::memory_order_acquire);
        if (!g || !g->active.load(std::memory_order_acquire)) return;
        std::unique_lock<std::mutex> lock(g->mutex, std::try_to_lock);
        if (!lock.owns_lock() || !g->active.load(std::memory_order_relaxed)) return;

        IniStamp new_stamp{};
        if (repair_cjk_font_ini(g->ini_path, g->font_path, /*allow_create=*/false,
                                /*force_font_path=*/false, &new_stamp) == FontRepairResult::Repaired) {
            g->last_known_stamp = new_stamp;
        }
        g->active.store(false, std::memory_order_release);
    } catch (...) {
    }
}

} // namespace openrr::pe::opti_plugin
