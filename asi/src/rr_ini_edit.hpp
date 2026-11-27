// Adapted from DLSSNR-AMD (MIT, Copyright (c) 2026 mochizuki0323).
#pragma once

#include "rr_path_win.hpp"
#include "rr_pe_config.hpp"

#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace openrr::ini_edit {

// In-place editor for openrr-amd.ini preserving comments, ordering, and key formatting.
class Ini {
public:
    explicit Ini(std::filesystem::path p) : path_(std::move(p)) {}

    bool refresh() {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        const std::string p_u8 = openrr::u8::text(path_);
        if (!openrr::u8::attributes_ex(p_u8, &d)) return false;
        const uint64_t s = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        if (s == stamp_ && !text_.empty()) return false;
        FILE* f = openrr::u8::fopen(p_u8.c_str(), "rb");
        if (!f) return false;
        std::string raw;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) raw.append(buf, n);
        const bool io_err = std::ferror(f) != 0;
        std::fclose(f);
        if (io_err) return false;
        openrr::u8::decode_ini_text(raw);
        stamp_ = s;
        text_ = std::move(raw);
        crlf_ = text_.find("\r\n") != std::string::npos;
        return true;
    }

    std::string get(const std::string& sec, const std::string& key, const std::string& def = {}) const {
        const std::string want_s = lower(sec), want_k = lower(key);
        std::string cur_s;
        for (size_t at = 0; at < text_.size();) {
            const size_t eol = text_.find('\n', at);
            const std::string line = text_.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            at = eol == std::string::npos ? text_.size() : eol + 1;
            const std::string t = trim(line);
            if (t.empty() || t.front() == ';' || t.front() == '#') continue;
            if (t.front() == '[') {
                const auto e = t.find(']');
                cur_s = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
                continue;
            }
            const auto eq = t.find('=');
            if (cur_s == want_s && eq != std::string::npos && lower(trim(t.substr(0, eq))) == want_k) {
                return trim(t.substr(eq + 1));
            }
        }
        return def;
    }

    void set(const std::string& sec, const std::string& key, const std::string& value) {
        const std::string want_s = lower(sec), want_k = lower(key);
        const std::string nl = crlf_ ? "\r\n" : "\n";
        std::string out, cur_s;
        bool done = false;
        for (size_t at = 0; at < text_.size();) {
            const size_t eol = text_.find('\n', at);
            const std::string line = text_.substr(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
            at = eol == std::string::npos ? text_.size() : eol + 1;
            const std::string t = trim(line);
            const bool is_comment = t.empty() || t.front() == ';' || t.front() == '#';
            if (!is_comment && t.front() == '[') {
                if (!done && cur_s == want_s) {
                    if (!out.empty() && out.back() != '\n') out += nl;
                    out += key + " = " + value + nl;
                    done = true;
                }
                const auto e = t.find(']');
                cur_s = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            } else if (!is_comment && cur_s == want_s) {
                const auto eq = t.find('=');
                if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == want_k) {
                    if (!done) {
                        out += key + " = " + value + (!line.empty() && line.back() == '\n' ? nl : "");
                        done = true;
                    }
                    continue;
                }
            }
            out += line;
        }
        if (!done) {
            if (!out.empty() && out.back() != '\n') out += nl;
            if (cur_s != want_s) {
                out += nl + "[" + sec + "]" + nl;
            }
            out += key + " = " + value + nl;
        }
        if (out != text_) {
            text_ = std::move(out);
            dirty_ = true;
        }
    }

    bool save(bool notify_config = true) {
        if (!dirty_) return true;
        const std::string p_u8 = openrr::u8::text(path_);
        const std::string tmp_u8 = p_u8 + ".openrr.tmp";
        FILE* f = openrr::u8::fopen(tmp_u8.c_str(), "wb");
        if (!f) return false;
        const bool wrote = text_.empty() || std::fwrite(text_.data(), 1, text_.size(), f) == text_.size();
        const bool closed = std::fclose(f) == 0;
        const std::wstring tmp_w = openrr::u8::wide(tmp_u8);
        const std::wstring dst_w = openrr::u8::wide(p_u8);
        if (!wrote || !closed || !MoveFileExW(tmp_w.c_str(), dst_w.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp_w.c_str());
            return false;
        }
        dirty_ = false;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (openrr::u8::attributes_ex(p_u8, &d))
            stamp_ = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        if (notify_config) openrr::pe::touch_config();
        return true;
    }

private:
    static std::string trim(const std::string& s) {
        const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::filesystem::path path_;
    std::string text_;
    uint64_t stamp_ = 0;
    bool crlf_ = true;
    bool dirty_ = false;
};

} // namespace openrr::ini_edit
