// SPDX-License-Identifier: MIT
// Adapted from mochi / DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#include "rr_pe_control.hpp"
#include "rr_path_win.hpp"
#include "rr_pe_config.hpp"
#include "rr_pe_log.hpp"

#include <windows.h>
#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrr::pe::control {
namespace {
openrr::control::Shared* shared();
}

openrr::control::Shared* page() { return shared(); }

namespace {

openrr::control::Shared* g_shared = nullptr;

openrr::control::Shared* shared() {
    static openrr::control::Shared* s = [] {
        auto* p = new openrr::control::Shared{};
        p->magic = openrr::control::kMagic;
        p->version = openrr::control::kVersion;
        g_shared = p;
        return p;
    }();
    return s;
}

void write_control_bin(const openrr::control::Shared* s) {
    if (!s) return;
    static const bool enabled = [] {
        const char* e = std::getenv("OPENRR_CONTROL_BIN");
        return e && e[0] == '1';
    }();
    if (!enabled) return;
    static ULONGLONG last_write = 0;
    const ULONGLONG now = GetTickCount64();
    if (last_write && now - last_write < 500) return;
    last_write = now;
    const std::string folder = module_folder();
    const std::string path = (folder.empty() ? std::string() : folder + "\\") + "openrr-control.bin";
    if (FILE* f = openrr::u8::fopen(path.c_str(), "wb")) {
        std::fwrite(s, 1, sizeof(*s), f);
        std::fclose(f);
    }
}

HWND g_window = nullptr;
WNDPROC g_previous = nullptr;

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using GetCursorInfoFn = BOOL(WINAPI*)(CURSORINFO*);
using KeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(BYTE*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);

ClipCursorFn g_real_clip = nullptr;
SetCursorPosFn g_real_setpos = nullptr;
GetCursorPosFn g_real_getpos = nullptr, g_real_physpos = nullptr;
GetCursorInfoFn g_real_cursorinfo = nullptr;
KeyStateFn g_real_async = nullptr, g_real_keystate = nullptr;
GetKeyboardStateFn g_real_keyboard = nullptr;
GetRawInputDataFn g_real_rawdata = nullptr;
GetRawInputBufferFn g_real_rawbuffer = nullptr;

using NtUserGetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using NtUserGetCursorPosFn = BOOL(WINAPI*)(POINT*, DWORD);
NtUserGetAsyncKeyStateFn g_nt_async = nullptr;
NtUserGetCursorPosFn g_nt_getpos = nullptr;

void init_win32u() {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE w32u = GetModuleHandleW(L"win32u.dll");
        if (!w32u) w32u = LoadLibraryW(L"win32u.dll");
        if (w32u) {
            g_nt_async = reinterpret_cast<NtUserGetAsyncKeyStateFn>(
                reinterpret_cast<void*>(GetProcAddress(w32u, "NtUserGetAsyncKeyState")));
            g_nt_getpos = reinterpret_cast<NtUserGetCursorPosFn>(
                reinterpret_cast<void*>(GetProcAddress(w32u, "NtUserGetCursorPos")));
        }
    });
}

// Bypass user32.dll hooks installed by OptiInput when OptiScaler's menu is open so both menus toggle and interact together.
SHORT raw_async_key_state(int vk) {
    init_win32u();
    static thread_local bool gui_init = false;
    if (!gui_init) {
        (void)(g_real_async ? g_real_async(VK_INSERT) : GetAsyncKeyState(VK_INSERT));
        gui_init = true;
    }
    if (g_nt_async) {
        int query_vk = vk;
        if ((vk == VK_LBUTTON || vk == VK_RBUTTON) && GetSystemMetrics(SM_SWAPBUTTON)) {
            query_vk = (vk == VK_LBUTTON) ? VK_RBUTTON : VK_LBUTTON;
        }
        return g_nt_async(query_vk);
    }
    return g_real_async ? g_real_async(vk) : GetAsyncKeyState(vk);
}

BOOL raw_get_cursor_pos(POINT* p) {
    init_win32u();
    if (g_nt_getpos) return g_nt_getpos(p, 1);
    return g_real_getpos ? g_real_getpos(p) : GetCursorPos(p);
}

std::mutex g_clip_mutex;
bool g_game_clipped = false;
RECT g_game_clip{};
POINT g_frozen{};
std::atomic<bool> g_menu_open{false};
std::atomic<DWORD> g_watch_thread{0};

bool held() { return g_menu_open.load() && GetCurrentThreadId() != g_watch_thread.load(); }

// Keys allowed through while the OpenRR menu is open (HANDOFF §4 & §5):
// - OptiScaler's [Menu] ShortcutKey (default Insert = VK_INSERT) when [OpenRR] PassOptiScalerKey = 1 (default)
// - mochi's [NRControl] MenuKey in dlssnr-amd.ini (default End = VK_END)
std::atomic<int> g_pass_vk_opti{VK_INSERT};
std::atomic<int> g_pass_vk_mochi{VK_END};

bool passed(int vk) {
    if (vk <= 0 || vk >= 256) return false;
    const int p1 = g_pass_vk_opti.load(std::memory_order_relaxed);
    const int p2 = g_pass_vk_mochi.load(std::memory_order_relaxed);
    return (p1 != 0 && vk == p1) || (p2 != 0 && vk == p2);
}

BOOL WINAPI clip_cursor(const RECT* r) {
    {
        std::lock_guard<std::mutex> lock(g_clip_mutex);
        g_game_clipped = r != nullptr;
        if (r) g_game_clip = *r;
    }
    return held() ? TRUE : g_real_clip(r);
}
BOOL WINAPI set_cursor_pos(int x, int y) { return held() ? TRUE : g_real_setpos(x, y); }
BOOL WINAPI get_cursor_pos(POINT* p) {
    if (!held() || !p) return g_real_getpos(p);
    *p = g_frozen;
    return TRUE;
}
BOOL WINAPI get_physical_cursor_pos(POINT* p) {
    if (!held() || !p) return g_real_physpos(p);
    *p = g_frozen;
    return TRUE;
}
BOOL WINAPI get_cursor_info(CURSORINFO* ci) {
    const BOOL ok = g_real_cursorinfo(ci);
    if (ok && ci && held()) ci->ptScreenPos = g_frozen;
    return ok;
}
SHORT WINAPI get_async_key_state(int vk) { return held() && !passed(vk) ? 0 : g_real_async(vk); }
SHORT WINAPI get_key_state(int vk) { return held() && !passed(vk) ? 0 : g_real_keystate(vk); }
BOOL WINAPI get_keyboard_state(BYTE* keys) {
    const BOOL ok = g_real_keyboard(keys);
    if (ok && keys && held()) {
        const int p1 = g_pass_vk_opti.load(std::memory_order_relaxed);
        const int p2 = g_pass_vk_mochi.load(std::memory_order_relaxed);
        const BYTE k1 = p1 > 0 && p1 < 256 ? keys[p1] : 0;
        const BYTE k2 = p2 > 0 && p2 < 256 ? keys[p2] : 0;
        std::memset(keys, 0, 256);
        if (p1 > 0 && p1 < 256) keys[p1] = k1;
        if (p2 > 0 && p2 < 256) keys[p2] = k2;
    }
    return ok;
}
UINT WINAPI get_raw_input_data(HRAWINPUT h, UINT command, void* data, UINT* size, UINT header) {
    const UINT r = g_real_rawdata(h, command, data, size, header);
    if (held() && data && command == RID_INPUT && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) {
        auto* in = static_cast<RAWINPUT*>(data);
        if (in->header.dwType == RIM_TYPEMOUSE) {
            in->data.mouse.lLastX = in->data.mouse.lLastY = 0;
            in->data.mouse.usButtonFlags = 0;
            in->data.mouse.usButtonData = 0;
        } else if (in->header.dwType == RIM_TYPEKEYBOARD && !passed(in->data.keyboard.VKey)) {
            in->data.keyboard.VKey = 0xFF;
            in->data.keyboard.MakeCode = 0;
            in->data.keyboard.Message = 0;
        }
    }
    return r;
}
UINT WINAPI get_raw_input_buffer(RAWINPUT* data, UINT* size, UINT header) {
    const UINT r = g_real_rawbuffer(data, size, header);
    return data && held() && r != UINT(-1) ? 0u : r;
}

bool hook_input() {
    static bool tried = false, ok = false;
    if (tried) return ok;
    tried = true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    struct Hook { const char* name; void* detour; void** real; } hooks[] = {
        {"ClipCursor", reinterpret_cast<void*>(&clip_cursor), reinterpret_cast<void**>(&g_real_clip)},
        {"SetCursorPos", reinterpret_cast<void*>(&set_cursor_pos), reinterpret_cast<void**>(&g_real_setpos)},
        {"GetCursorPos", reinterpret_cast<void*>(&get_cursor_pos), reinterpret_cast<void**>(&g_real_getpos)},
        {"GetPhysicalCursorPos", reinterpret_cast<void*>(&get_physical_cursor_pos), reinterpret_cast<void**>(&g_real_physpos)},
        {"GetCursorInfo", reinterpret_cast<void*>(&get_cursor_info), reinterpret_cast<void**>(&g_real_cursorinfo)},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&get_async_key_state), reinterpret_cast<void**>(&g_real_async)},
        {"GetKeyState", reinterpret_cast<void*>(&get_key_state), reinterpret_cast<void**>(&g_real_keystate)},
        {"GetKeyboardState", reinterpret_cast<void*>(&get_keyboard_state), reinterpret_cast<void**>(&g_real_keyboard)},
        {"GetRawInputData", reinterpret_cast<void*>(&get_raw_input_data), reinterpret_cast<void**>(&g_real_rawdata)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&get_raw_input_buffer), reinterpret_cast<void**>(&g_real_rawbuffer)},
    };
    const MH_STATUS init = MH_Initialize();
    ok = user32 && (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED);
    std::vector<void*> done;
    for (const Hook& h : hooks) {
        void* at = ok ? reinterpret_cast<void*>(GetProcAddress(user32, h.name)) : nullptr;
        if (!at) continue;
        if (std::find(done.begin(), done.end(), at) != done.end()) continue;
        if (MH_CreateHook(at, h.detour, h.real) == MH_OK && MH_EnableHook(at) == MH_OK) done.push_back(at);
        else log("[openrr] OpenRR menu: %s could not be hooked; the game may still see it under the menu", h.name);
    }
    ok = ok && g_real_clip && g_real_setpos && g_real_getpos;
    if (!g_real_physpos) g_real_physpos = g_real_getpos;
    return ok;
}

void menu_changed(bool open) {
    if (!hook_input()) { g_menu_open.store(open); return; }
    if (open) {
        raw_get_cursor_pos(&g_frozen);
        g_menu_open.store(true);
        g_real_clip(nullptr);
    } else {
        g_menu_open.store(false);
        std::lock_guard<std::mutex> lock(g_clip_mutex);
        g_real_clip(g_game_clipped ? &g_game_clip : nullptr);
        g_real_setpos(g_frozen.x, g_frozen.y);
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    openrr::control::Shared* s = g_shared;
    if (s && s->wayland) {
        if (msg == WM_MOUSEWHEEL) s->wheel += GET_WHEEL_DELTA_WPARAM(wp);
        if (msg == WM_CHAR && s->menu_open && wp >= 32) {
            s->chars[s->chars_written % 64] = uint32_t(wp);
            ++s->chars_written;
        }
        if (s->menu_open) {
            const bool pass = (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
                              passed(int(wp));
            if (!pass) {
                if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);
                if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
            }
        }
    }
    return CallWindowProcW(g_previous, hwnd, msg, wp, lp);
}

HWND game_window() {
    struct Find { HWND best = nullptr; long area = 0; } f;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto& f = *reinterpret_cast<Find*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        RECT r{};
        if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClientRect(h, &r)) {
            const long area = long(r.right - r.left) * long(r.bottom - r.top);
            if (area > f.area) { f.area = area; f.best = h; }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

void read_pass_key() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000) return;
    last = now;
    const std::string folder = module_folder();
    char v[32] = {};
    // Default PassOptiScalerKey = "1" per HANDOFF §5
    openrr::u8::profile("OpenRR", "PassOptiScalerKey", "1", v, sizeof v, folder + "\\openrr-amd.ini");
    int opti_vk = 0;
    if (std::atoi(v) == 1 || !_stricmp(v, "true") || !_stricmp(v, "on") || !_stricmp(v, "yes")) {
        opti_vk = read_optiscaler_shortcut_key(folder);
    }
    const int mochi_vk = read_mochi_menu_key(folder);
    if (opti_vk != g_pass_vk_opti.exchange(opti_vk)) {
        if (opti_vk)
            log("[openrr] OpenRR menu: OptiScaler's menu key (%s, 0x%02X) reaches OptiScaler while OpenRR is open",
                vk_name(opti_vk).c_str(), opti_vk);
    }
    g_pass_vk_mochi.store(mochi_vk, std::memory_order_relaxed);
}

void watch() {
    g_watch_thread.store(GetCurrentThreadId());
    for (;;) {
        read_pass_key();
        openrr::control::Shared* s = g_shared;
        const bool open = s && s->menu_open != 0;
        if (s && open != g_menu_open.load()) menu_changed(open);
        if (open && g_real_clip) {
            RECT clip{}, screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
            screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (GetClipCursor(&clip) && !EqualRect(&clip, &screen)) g_real_clip(nullptr);
        }
        if (s) {
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &pid);
            s->game_focused = fg && pid == GetCurrentProcessId() ? 1u : 0u;
        }
        if (s && s->wayland) {
            if (!g_window || !IsWindow(g_window)) {
                g_window = game_window();
                if (g_window) {
                    g_previous = reinterpret_cast<WNDPROC>(
                        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
                    log("[openrr] OpenRR menu: input comes from the game's window");
                }
            }
            if (g_window) {
                CURSORINFO ci{};
                ci.cbSize = sizeof ci;
                s->cursor_shown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor ? 1u : 0u;
                uint32_t keys[8] = {};
                const bool focused = GetForegroundWindow() == g_window;
                for (int vk = 1; vk < 256; ++vk)
                    if (focused && (raw_async_key_state(vk) & 0x8000))
                        keys[vk / 32] |= 1u << (vk % 32);
                std::memcpy(s->keys, keys, sizeof keys);
                POINT p{};
                RECT r{};
                if (raw_get_cursor_pos(&p) && ScreenToClient(g_window, &p)) {
                    s->mouse_x = p.x;
                    s->mouse_y = p.y;
                }
                if (GetClientRect(g_window, &r)) {
                    s->client_w = uint32_t(r.right);
                    s->client_h = uint32_t(r.bottom);
                }
                s->buttons = (keys[0] & (1u << VK_LBUTTON) ? 1u : 0u) | (keys[0] & (1u << VK_RBUTTON) ? 2u : 0u) |
                             (keys[0] & (1u << VK_MBUTTON) ? 4u : 0u);
                ++s->input_seq;
            }
        }
        Sleep(s && (s->wayland || open) ? 4 : 50);
    }
}

std::once_flag g_watch_started;
void start_watch_on(openrr::control::Shared* s) {
    (void)s;
    std::call_once(g_watch_started, [] {
        std::thread(watch).detach();
    });
}

} // namespace

void start_input() {
    if (openrr::control::Shared* s = shared()) start_watch_on(s);
}

void asi_facts(const char* build) {
    openrr::control::Shared* s = shared();
    if (!s) return;
    if (build) std::snprintf(s->asi_build, sizeof s->asi_build, "%s", build);
}

void report(const Status& st) {
    openrr::control::Shared* s = shared();
    if (!s) return;
    start_watch_on(s);
    s->route = st.route;
    s->rr_active = st.rr_active ? 1u : 0u;
    s->state = st.state;
    s->backend = st.backend;
    s->fast_mode = st.fast_mode;
    s->frames = st.frames;
    s->gpu_ms_last = st.gpu_ms_last;
    s->gpu_ms_average = st.gpu_ms_average;
    s->gpu_raw_last = st.gpu_raw_last;
    s->gpu_raw_p50 = st.gpu_raw_p50;
    s->gpu_raw_p95 = st.gpu_raw_p95;
    s->render_width = st.render_width;
    s->render_height = st.render_height;
    s->output_width = st.output_width;
    s->output_height = st.output_height;
    s->coexist_mochi = st.coexist_mochi ? 1u : 0u;
    s->split_active = st.split_active ? 1u : 0u;
    std::snprintf(s->weights_source, sizeof s->weights_source, "%s", st.weights_source.c_str());
    std::snprintf(s->message, sizeof s->message, "%s", st.message.c_str());
    std::snprintf(s->backend_message, sizeof s->backend_message, "%s", st.backend_message.c_str());
    ++s->status_seq;
    write_control_bin(s);
}

} // namespace openrr::pe::control
