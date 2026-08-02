#pragma once
#include <array>
#include <cstdint>
#include <string_view>

// Frozen contract-1 model selected by production_codec_study.py. Symbol order
// is part of the wire grammar; do not reorder literals, tokens, or lengths.
namespace OverlayIdentity::Model {
inline constexpr std::array<uint8_t,47> UrlDirect{{
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,
    48,49,50,51,52,53,54,55,56,57,45,46,95,126,58,47,63,38,61,37,35
}};
inline constexpr std::array<std::string_view,18> UrlTokens{{
    ".com/",
    ".org/",
    ".net/",
    ".app/",
    ".dev/",
    ".io/",
    ".ini",
    ".txt",
    "download",
    "manifest",
    "release",
    "version",
    "current",
    "latest",
    "update",
    "config",
    "1kb",
    "1KB",
}};
// URL symbols: direct bytes above, UPPER, tokens above, ESCAPE, COPY, EOF.
inline constexpr std::array<uint8_t,69> UrlLengths{{
    4,6,5,5,4,6,6,6,4,9,6,4,6,5,4,5,9,4,4,4,6,6,5,9,7,9,
    11,9,10,9,11,12,11,12,12,14,5,5,14,15,13,5,11,12,10,15,14,14,
    7,10,9,8,9,9,8,7,8,10,8,9,10,9,7,11,11,11,14,6,5
}};
inline constexpr std::string_view GitHubAlphabet = "-.0123456789_abcdefghijklmnopqrstuvwxyz";
inline constexpr std::array<uint8_t,46> GitHubLengths{{
    5,9,9,8,8,8,12,10,10,10,9,9,8,4,6,5,5,4,6,6,5,4,8,6,5,5,5,4,5,8,4,4,4,5,7,7,8,6,8,3,7,12,7,12,8,12
}};
}
