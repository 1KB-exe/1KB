#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OverlayIdentity {

constexpr uint8_t KindMask = 0xc0;
constexpr uint8_t LengthMask = 0x3f;
constexpr uint8_t GitHubKind = 0x00;
constexpr uint8_t HttpsKind = 0x40;
constexpr uint8_t HttpKind = 0x80;
constexpr uint8_t BuiltInOneKB = 0xc0;
constexpr uint8_t ReservedKind = 0xc0;
constexpr uint8_t ExtendedLength = 0x3f;
inline constexpr wchar_t OneKBIdentity[] = L"gh:1KB-exe/1KB";
constexpr size_t MaxEncodedBodyBytes = 255;

// The validator is the application's authoritative canonical-identity parser.
// It returns the canonical form in normalized. Encoding and decoding require
// normalized to equal the supplied/reconstructed identity.
using Validator = bool (*)(const std::wstring& supplied, std::wstring& normalized);

struct Decoded {
    std::wstring identity;
    size_t encodedBodyOffset = 0;
    size_t encodedBodyBytes = 0;
    size_t secretOffset = 0;
    size_t trailerBytes = 0;
};

// Version-1 bodies use frozen canonical Huffman models. GitHub fields support
// owner/repository COPY commands and exact-value shortcuts; URL bodies support
// approved compositional tokens and within-value overlapping COPY commands.
bool EncodeOverlayIdentity(const std::wstring& canonicalIdentity,
                           const uint8_t* secret, size_t secretBytes,
                           Validator validator, std::vector<uint8_t>& overlay);

// data may include bytes before the overlay. secretBytes identifies the public
// (zero) or private (8) physical layout; the typed trailer is always at EOF.
bool DecodeOverlayIdentity(const uint8_t* data, size_t dataBytes,
                           size_t secretBytes, size_t maxCanonicalBytes,
                           Validator validator, Decoded& decoded);

} // namespace OverlayIdentity
