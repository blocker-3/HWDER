// Adds the installed game to Steam as a non-Steam shortcut (userdata/<account>/config/shortcuts.vdf).
// Pure byte functions here (tested by tools/tests/steam_shortcut_test.cpp); bootstrap.cpp finds the file,
// closes a running Steam first (Steam rewrites the file from memory when it closes) and keeps a backup.
//
// shortcuts.vdf is Valve's binary KeyValues: 0x00 name\0 {children} 0x08 is a map, 0x01 name\0 value\0 a
// string, 0x02 name\0 <int32 LE> an integer (0x07: a 64-bit one). The file is one map "shortcuts" whose
// children are maps named "0", "1", ..., then a final 0x08.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace steam {

struct Shortcut {
    std::string name, exe, start_dir, icon;  // UTF-8; exe and start_dir unquoted
};

// Steam's id for a non-Steam shortcut: CRC-32 of the quoted exe plus the name, top bit set.
inline uint32_t shortcut_appid(const std::string& quoted_exe, const std::string& name) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : quoted_exe + name) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc | 0x80000000u;
}

namespace detail {

struct Node {
    unsigned char type = 0;
    std::string key, value;
    std::vector<Node> kids;
};

// Reads the children of a map up to and including its closing 0x08.
inline bool parse_map(const std::string& b, size_t& at, Node& map, int depth) {
    if (depth > 16) return false;
    while (at < b.size()) {
        Node n;
        n.type = (unsigned char)b[at++];
        if (n.type == 0x08) return true;
        size_t end = b.find('\0', at);
        if (end == std::string::npos) return false;
        n.key = b.substr(at, end - at);
        at = end + 1;
        if (n.type == 0x00) {
            if (!parse_map(b, at, n, depth + 1)) return false;
        } else if (n.type == 0x01) {
            end = b.find('\0', at);
            if (end == std::string::npos) return false;
            n.value = b.substr(at, end - at);
            at = end + 1;
        } else if (n.type == 0x02 || n.type == 0x07) {
            const size_t bytes = n.type == 0x02 ? 4 : 8;
            if (at + bytes > b.size()) return false;
            n.value = b.substr(at, bytes);
            at += bytes;
        } else {
            return false;
        }
        map.kids.push_back(std::move(n));
    }
    return false;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string unquoted(const std::string& s) {
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}
inline void str(std::string& o, const char* k, const std::string& v) {
    o += '\x01';
    o += k;
    o += '\0';
    o += v;
    o += '\0';
}
inline void num(std::string& o, const char* k, uint32_t v) {
    o += '\x02';
    o += k;
    o += '\0';
    for (int i = 0; i < 4; ++i) o += (char)((v >> (8 * i)) & 0xFF);
}

}  // namespace detail

enum class Added { Yes, AlreadyThere, Unreadable };

// `existing` is the current file ("" when there is none); on Yes, `out` is the new file.
inline Added add(const std::string& existing, const Shortcut& s, std::string& out) {
    static const std::string head("\0shortcuts\0", 11);
    const std::string base = existing.empty() ? head + "\x08\x08" : existing;
    if (base.compare(0, head.size(), head) != 0) return Added::Unreadable;
    size_t at = head.size();
    detail::Node list;
    if (!detail::parse_map(base, at, list, 0) || at + 1 != base.size() || base[at] != '\x08') return Added::Unreadable;
    unsigned next = 0;
    for (const auto& kid : list.kids) {
        if (kid.type != 0x00) continue;
        unsigned index = 0;
        bool digits = !kid.key.empty();
        for (char c : kid.key) digits = digits && c >= '0' && c <= '9', index = index * 10 + (c - '0');
        if (digits && index + 1 > next) next = index + 1;
        for (const auto& field : kid.kids)
            if (field.type == 0x01 && detail::lower(field.key) == "exe" &&
                detail::lower(detail::unquoted(field.value)) == detail::lower(s.exe))
                return Added::AlreadyThere;
    }
    const std::string quoted = "\"" + s.exe + "\"";
    std::string e;
    e += '\0';
    e += std::to_string(next);
    e += '\0';
    detail::num(e, "appid", shortcut_appid(quoted, s.name));
    detail::str(e, "AppName", s.name);
    detail::str(e, "Exe", quoted);
    detail::str(e, "StartDir", "\"" + s.start_dir + "\"");
    detail::str(e, "icon", s.icon);
    detail::str(e, "ShortcutPath", "");
    detail::str(e, "LaunchOptions", "");
    detail::num(e, "IsHidden", 0);
    detail::num(e, "AllowDesktopConfig", 1);
    detail::num(e, "AllowOverlay", 1);
    detail::num(e, "OpenVR", 0);
    detail::num(e, "Devkit", 0);
    detail::str(e, "DevkitGameID", "");
    detail::num(e, "DevkitOverrideAppID", 0);
    detail::num(e, "LastPlayTime", 0);
    detail::str(e, "FlatpakAppID", "");
    e += '\0';
    e += "tags";
    e += '\0';
    e += '\x08';  // end of tags
    e += '\x08';  // end of this shortcut
    // base ends with the list's closing 0x08 and the file's final 0x08: insert before both.
    out = base.substr(0, base.size() - 2) + e + "\x08\x08";
    return Added::Yes;
}

}  // namespace steam
