#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

inline std::size_t utf8_len(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

inline std::string utf8_prefix(std::string_view s, std::size_t n) {
    std::size_t i = 0;
    std::size_t count = 0;
    while (i < s.size() && count < n) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t w = 1;
        if ((c & 0x80) == 0x00) {
            w = 1;
        } else if ((c & 0xE0) == 0xC0) {
            w = 2;
        } else if ((c & 0xF0) == 0xE0) {
            w = 3;
        } else if ((c & 0xF8) == 0xF0) {
            w = 4;
        }
        if (i + w > s.size()) {
            w = s.size() - i;
        }
        i += w;
        ++count;
    }
    return std::string(s.substr(0, i));
}

inline std::string utf8_tail(std::string_view s, std::size_t n) {
    if (n == 0) {
        return {};
    }
    std::size_t seen = 0;
    std::size_t i = s.size();
    while (i > 0 && seen < n) {
        --i;
        auto c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            ++seen;
        }
    }
    return std::string(s.substr(i));
}

inline bool is_ascii_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Trim ASCII whitespace plus NBSP (U+00A0) and ideographic space (U+3000).
inline std::string trim_copy(std::string s) {
    auto width_at = [](const std::string& str, std::size_t i) -> std::size_t {
        auto c = static_cast<unsigned char>(str[i]);
        if (is_ascii_ws(c)) {
            return 1;
        }
        if (c == 0xC2 && i + 1 < str.size() && static_cast<unsigned char>(str[i + 1]) == 0xA0) {
            return 2;
        }
        if (c == 0xE3 && i + 2 < str.size() && static_cast<unsigned char>(str[i + 1]) == 0x80 &&
            static_cast<unsigned char>(str[i + 2]) == 0x80) {
            return 3;
        }
        return 0;
    };
    std::size_t b = 0;
    while (b < s.size()) {
        std::size_t w = width_at(s, b);
        if (w == 0) {
            break;
        }
        b += w;
    }
    std::size_t e = s.size();
    while (e > b) {
        std::size_t i = e - 1;
        while (i > b && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            --i;
        }
        std::size_t w = width_at(s, i);
        if (w == 0 || i + w != e) {
            break;
        }
        e = i;
    }
    return s.substr(b, e - b);
}
