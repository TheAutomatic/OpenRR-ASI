#pragma once
// The menu's language files: openrr-amd\lang\<name>.txt, chosen by [OpenRR] Language (empty: English, the texts as
// written in the code). The package brings English.txt, made from the code by asi/build/make_lang.py; a copy with its
// msgstr lines translated is a new language.
//
//   msgid "Neural Rendering"          the English text, exactly as in English.txt
//   msgstr "神经渲染"                 its translation; empty or missing: the English text
//
// "\n", "\"", "\\" and "\t" as in C; a line of only a "..." string continues the msgid / msgstr above it. Two entries
// are the file's own: msgid "@language" (its name in the menu) and msgid "@font" (a font file for its letters, in the
// lang folder or a full path; OpenRR's own font has Latin, Greek and Cyrillic letters only).
//
// A translation whose printf conversions (%d, %.2f, %s, ...) are not the English text's, in the same order, is not
// used: the menu passes it the English text's arguments (a bad language file cannot crash a snprintf). The file is
// read again when it changes (checked once a second at most).
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "rr_path_win.hpp"

namespace openrr::lang {

struct Table {
    std::string name;                 // the file's name without .txt ("" while none is loaded)
    std::string display, font;        // @language, @font
    std::deque<std::string> store;    // every text below points into this
    std::unordered_map<std::string_view, std::string_view> map;      // English -> translation
    std::unordered_map<std::string_view, std::string_view> derived;  // "Label##id", combo item lists, made once
    std::string letters;              // every translation, for the font's glyphs
    std::vector<unsigned> code_points; // the letters outside ASCII, each once (the font's glyph set)
    int entries = 0, rejected = 0;    // translations used / left English (their conversions differ)
};
inline Table& table() { static Table t; return t; }
// True while the font in use lacks letters of the loaded language (set by the menu when the fallback found no
// glyphs): the texts stay English rather than empty boxes.
inline bool& suspended() { static bool s = false; return s; }

// The printf conversions of a text, in order ("%d%.2f%s"), flags and widths kept: a translation must have the same.
// (No space flag: "100 % only" in a text is a percent sign, not "% o".)
inline std::string conversions(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        const size_t start = i++;
        while (i < s.size() && std::strchr("-+#0123456789.*hljztL", s[i])) ++i;
        // a conversion only when it ends in one of printf's letters ("100 %)" is a percent sign in the text)
        if (i < s.size() && std::strchr("diouxXeEfgGaAcspn%", s[i])) out.append(s.substr(start, i - start + 1));
        else i = start;
    }
    return out;
}

// One quoted string's contents, its escapes resolved; false when the line holds none.
inline bool unquote(const std::string& line, size_t from, std::string* out) {
    const size_t q = line.find('"', from);
    if (q == std::string::npos) return false;
    const size_t end = line.rfind('"');
    if (end <= q) return false;
    for (size_t i = q + 1; i < end; ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < end) {
            const char e = line[++i];
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' : e;
        }
        *out += c;
    }
    return true;
}

// The file's entries: (msgid, msgstr), in order.
inline std::vector<std::pair<std::string, std::string>> read_file(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> entries;
    FILE* f = openrr::u8::fopen(path.c_str(), "rb");
    if (!f) return entries;
    std::string text;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a BOM (Notepad's UTF-8)
    std::string id, str;
    int in = 0;   // 1 msgid, 2 msgstr
    bool have = false;
    const auto flush = [&] {
        if (have) entries.emplace_back(id, str);
        id.clear(); str.clear(); have = false; in = 0;
    };
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') { if (b == std::string::npos && in == 2) flush(); continue; }
        if (line.compare(b, 5, "msgid") == 0) {
            flush();
            in = 1; have = true;
            unquote(line, b + 5, &id);
        } else if (line.compare(b, 6, "msgstr") == 0) {
            in = 2;
            unquote(line, b + 6, &str);
        } else if (line[b] == '"') {
            if (in == 1) unquote(line, b, &id);
            else if (in == 2) unquote(line, b, &str);
        }
    }
    flush();
    return entries;
}

// Loads `folder`\`name`.txt (empty name: English). Returns false when the file cannot be read (English then).
inline bool load(const std::string& folder, const std::string& name) {
    Table& t = table();
    t = Table{};
    if (name.empty()) return true;
    const auto entries = read_file(folder + "\\" + name + ".txt");
    if (entries.empty()) return false;
    t.name = name;
    for (const auto& [id, str] : entries) {
        if (id == "@language") { t.display = str; continue; }
        if (id == "@font") { t.font = str; continue; }
        if (id.empty() || str.empty() || id == str) continue;
        if (conversions(id) != conversions(str)) { ++t.rejected; continue; }
        const std::string& k = t.store.emplace_back(id);
        const std::string& v = t.store.emplace_back(str);
        if (t.map.emplace(k, v).second) { ++t.entries; t.letters += str; }
    }
    // the letters outside ASCII, from the UTF-8 (the BMP only: what ImGui's glyphs cover)
    std::vector<bool> seen(0x10000, false);
    for (size_t i = 0; i < t.letters.size();) {
        const unsigned char c = static_cast<unsigned char>(t.letters[i]);
        unsigned cp = 0;
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > t.letters.size()) break;
        cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(t.letters[i + k]) & 0x3Fu);
        i += n;
        if (cp >= 0x80 && cp < 0x10000 && !seen[cp]) { seen[cp] = true; t.code_points.push_back(cp); }
    }
    return true;
}

// The text in the language chosen, else as given. "Label##id": the label's translation, the ImGui id kept.
inline const char* tr(const char* en) {
    Table& t = table();
    if (t.map.empty() || suspended() || !en || !*en) return en;
    const std::string_view v(en);
    if (const auto it = t.map.find(v); it != t.map.end()) return it->second.data();
    const size_t hash = v.find("##");
    if (hash == std::string_view::npos || hash == 0) return en;
    if (const auto it = t.derived.find(v); it != t.derived.end()) return it->second.data();
    const auto head = t.map.find(v.substr(0, hash));
    if (head == t.map.end()) return en;
    const std::string& k = t.store.emplace_back(v);
    const std::string& d = t.store.emplace_back(std::string(head->second) + std::string(v.substr(hash)));
    t.derived.emplace(k, d);
    return d.c_str();
}
inline std::string tr(const std::string& en) { return tr(en.c_str()); }
// A combo's items, NUL-separated and ended by an empty one (ImGui::Combo's form), each translated.
inline const char* tr_items(const char* items) {
    Table& t = table();
    if (t.map.empty() || suspended() || !items) return items;
    size_t n = 0;
    while (items[n]) n += std::strlen(items + n) + 1;
    const std::string_view v(items, n + 1);
    if (const auto it = t.derived.find(v); it != t.derived.end()) return it->second.data();
    std::string out;
    for (const char* p = items; *p; p += std::strlen(p) + 1) { out += tr(p); out += '\0'; }
    out += '\0';
    const std::string& k = t.store.emplace_back(v);
    const std::string& d = t.store.emplace_back(out);
    t.derived.emplace(k, d);
    return d.data();
}

// The language files in `folder`: (file name without .txt, the name to show), English first (no file needed).
inline std::vector<std::pair<std::string, std::string>> list(const std::string& folder) {
    std::vector<std::pair<std::string, std::string>> out{{"", "English"}};
    openrr::u8::list(folder + "\\*.txt", [&](const std::string& file, DWORD attributes) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return;
        std::string stem = file.substr(0, file.size() - 4);
        std::string lower = stem;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "readme" || lower == "english") return;   // English: the code's own texts
        std::string shown = stem;
        for (const auto& [id, str] : read_file(folder + "\\" + file))
            if (id == "@language") { if (!str.empty()) shown = str; break; }
        out.emplace_back(stem, shown);
    });
    return out;
}

// Each frame from the menu: `name` = [OpenRR] Language. Loads it when it changed or its file did (checked once a
// second). True when the texts changed this frame (the menu makes its font again for the new letters).
inline bool sync(const std::string& folder, const std::string& name) {
    static std::string loaded = "\x01";
    static FILETIME stamp{};
    static ULONGLONG checked = 0;
    const ULONGLONG now = GetTickCount64();
    FILETIME ft{};
    if (name == loaded) {
        if (name.empty() || now - checked < 1000) return false;
        checked = now;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!openrr::u8::attributes_ex((folder + "\\" + name + ".txt").c_str(), &a)) return false;
        if (CompareFileTime(&a.ftLastWriteTime, &stamp) == 0) return false;
        ft = a.ftLastWriteTime;
    } else {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!name.empty() && openrr::u8::attributes_ex((folder + "\\" + name + ".txt").c_str(), &a))
            ft = a.ftLastWriteTime;
    }
    loaded = name;
    stamp = ft;
    checked = now;
    load(folder, name);
    suspended() = false;   // checked again against the font
    return true;
}

}  // namespace openrr::lang
