// Environment option tokens use ASCII, independent of the host CRT and locale.
#pragma once

namespace host {
inline bool ascii_iequals(const char* a, const char* b) {
    if (!a || !b) return false;
    for (;; ++a, ++b) {
        const auto lower = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        };
        if (lower(static_cast<unsigned char>(*a)) != lower(static_cast<unsigned char>(*b))) return false;
        if (!*a) return true;
    }
}
}  // namespace host
