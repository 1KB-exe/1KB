#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "overlay-identity.h"
#include "overlay-identity-model.h"

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <string>
#include <string_view>
#include <windows.h>

namespace OverlayIdentity {
namespace {
constexpr unsigned UrlUpper = static_cast<unsigned>(Model::UrlDirect.size());
constexpr unsigned UrlTokenBase = UrlUpper + 1;
constexpr unsigned UrlEscape = UrlTokenBase + static_cast<unsigned>(Model::UrlTokens.size());
constexpr unsigned UrlCopy = UrlEscape + 1;
constexpr unsigned UrlEnd = UrlCopy + 1;
constexpr unsigned GhEnd = static_cast<unsigned>(Model::GitHubAlphabet.size());
constexpr unsigned GhCopyOwner = GhEnd + 1;
constexpr unsigned GhCopyRepository = GhCopyOwner + 1;
constexpr unsigned GhExactOwner = GhCopyRepository + 1;
constexpr unsigned GhExactRepository = GhExactOwner + 1;
constexpr unsigned GhWholeOwner = GhExactRepository + 1;
constexpr unsigned GhWholeRepository = GhWholeOwner + 1;
constexpr unsigned MaxCodeBits = 15;

bool Utf8(const std::wstring& value, std::vector<uint8_t>& bytes) {
    if (value.empty() || value.size() > INT_MAX) return false;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    bytes.resize(static_cast<size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), reinterpret_cast<char*>(bytes.data()),
                               count, nullptr, nullptr) == count;
}

bool Wide(const std::vector<uint8_t>& bytes, std::wstring& value) {
    if (bytes.empty() || bytes.size() > INT_MAX ||
        std::find(bytes.begin(), bytes.end(), uint8_t{0}) != bytes.end()) return false;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return false;
    value.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               reinterpret_cast<const char*>(bytes.data()),
                               static_cast<int>(bytes.size()), value.data(), count) == count;
}

bool Canonical(const std::wstring& value, Validator validator) {
    std::wstring normalized;
    return validator && validator(value, normalized) && normalized == value;
}

struct BitWriter {
    std::vector<uint8_t> bytes;
    size_t bits = 0;
    void Put(unsigned value, unsigned count) {
        while (count--) {
            if (!(bits & 7)) bytes.push_back(0);
            bytes.back() |= static_cast<uint8_t>(((value >> count) & 1) << (7 - (bits & 7)));
            ++bits;
        }
    }
};

struct BitReader {
    const uint8_t* data;
    size_t bytes, bit = 0;
    bool Bit(unsigned& value) {
        if (bit >= bytes * 8) return false;
        value = (data[bit / 8] >> (7 - bit % 8)) & 1; ++bit; return true;
    }
    bool Padding() {
        if (bytes * 8 - bit >= 8) return false;
        unsigned value;
        while (bit < bytes * 8) if (!Bit(value) || value) return false;
        return true;
    }
};

template<size_t N> struct Huffman {
    const std::array<uint8_t, N>& lengths;
    void Put(BitWriter& writer, unsigned symbol) const {
        if (symbol >= N || !lengths[symbol]) return;
        std::array<unsigned, MaxCodeBits + 1> counts{};
        for (uint8_t n : lengths) ++counts[n];
        unsigned code = 0, length = lengths[symbol];
        for (unsigned n = 1; n < length; ++n) code = (code + counts[n]) << 1;
        for (unsigned i = 0; i < symbol; ++i) if (lengths[i] == length) ++code;
        writer.Put(code, length);
    }
    bool Get(BitReader& reader, unsigned& symbol) const {
        std::array<unsigned, MaxCodeBits + 1> counts{};
        for (uint8_t n : lengths) {
            if (!n || n > MaxCodeBits) return false;
            ++counts[n];
        }
        unsigned code = 0, first = 0, next;
        for (unsigned length = 1; length <= MaxCodeBits; ++length) {
            if (!reader.Bit(next)) return false;
            code = (code << 1) | next;
            if (length > 1) first = (first + counts[length - 1]) << 1;
            if (code >= first && code - first < counts[length]) {
                unsigned rank = code - first;
                for (unsigned i = 0; i < N; ++i)
                    if (lengths[i] == length && !rank--) { symbol = i; return true; }
                return false;
            }
        }
        return false;
    }
};

int UrlLiteralSymbol(unsigned byte) {
    auto found = std::find(Model::UrlDirect.begin(), Model::UrlDirect.end(), byte);
    return found == Model::UrlDirect.end() ? -1 : static_cast<int>(found - Model::UrlDirect.begin());
}
unsigned UrlLiteralBits(unsigned byte) {
    int symbol = UrlLiteralSymbol(byte);
    if (symbol >= 0) return Model::UrlLengths[static_cast<unsigned>(symbol)];
    return byte >= 'A' && byte <= 'Z' ? Model::UrlLengths[UrlUpper] + 5 : Model::UrlLengths[UrlEscape] + 8;
}

unsigned GammaBits(unsigned value) {
    unsigned bits = 1;
    while (value >>= 1) bits += 2;
    return bits;
}
void PutGamma(BitWriter& writer, unsigned value) { writer.Put(value, GammaBits(value)); }
bool GetGamma(BitReader& reader, unsigned& value) {
    unsigned zeros = 0, bit;
    while (true) {
        if (!reader.Bit(bit)) return false;
        if (bit) break;
        if (++zeros >= 16) return false;
    }
    value = 1;
    while (zeros--) { if (!reader.Bit(bit)) return false; value = (value << 1) | bit; }
    return true;
}

struct Action { enum Kind : uint8_t { Literal, Token, Copy, End, Exact, Whole } kind; unsigned value, length; };

bool EncodeUrl(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    const Huffman<Model::UrlLengths.size()> h{Model::UrlLengths};
    const size_t n = input.size();
    std::vector<unsigned> best(n + 1, std::numeric_limits<unsigned>::max());
    std::vector<Action> action(n); best[n] = Model::UrlLengths[UrlEnd];
    for (size_t position = n; position-- > 0;) {
        unsigned byte = input[position];
        best[position] = UrlLiteralBits(byte) + best[position + 1];
        action[position] = {Action::Literal, byte, 1};
        for (unsigned id = 0; id < Model::UrlTokens.size(); ++id) {
            auto token = Model::UrlTokens[id];
            if (token.size() <= n - position && std::equal(token.begin(), token.end(), input.begin() + position)) {
                unsigned cost = Model::UrlLengths[UrlTokenBase + id] + best[position + token.size()];
                if (cost < best[position] || (cost == best[position] && token.size() > action[position].length)) {
                    best[position] = cost; action[position] = {Action::Token, id, static_cast<unsigned>(token.size())};
                }
            }
        }
        for (unsigned distance = 1; distance <= position; ++distance) {
            unsigned match = 0;
            while (position + match < n && input[position + match] == input[position + match - distance]) ++match;
            unsigned literal = 0;
            for (unsigned length = 1; length <= match; ++length) {
                literal += UrlLiteralBits(input[position + length - 1]);
                if (length < 3) continue;
                unsigned operation = Model::UrlLengths[UrlCopy] + GammaBits(distance) + GammaBits(length - 2);
                if (operation >= literal) continue;
                unsigned cost = operation + best[position + length];
                if (cost < best[position] || (cost == best[position] &&
                    (length > action[position].length || (length == action[position].length && action[position].kind != Action::Copy)))) {
                    best[position] = cost; action[position] = {Action::Copy, distance, length};
                }
            }
        }
    }
    BitWriter writer;
    for (size_t position = 0; position < n;) {
        auto a = action[position];
        if (a.kind == Action::Literal) {
            int symbol = UrlLiteralSymbol(a.value);
            if (symbol >= 0) h.Put(writer, static_cast<unsigned>(symbol));
            else if (a.value >= 'A' && a.value <= 'Z') { h.Put(writer, UrlUpper); writer.Put(a.value - 'A', 5); }
            else { h.Put(writer, UrlEscape); writer.Put(a.value, 8); }
        } else if (a.kind == Action::Token) h.Put(writer, UrlTokenBase + a.value);
        else { h.Put(writer, UrlCopy); PutGamma(writer, a.value); PutGamma(writer, a.length - 2); }
        position += a.length;
    }
    h.Put(writer, UrlEnd);
    if (writer.bytes.empty() || writer.bytes.size() > MaxEncodedBodyBytes) return false;
    output = std::move(writer.bytes); return true;
}

bool DecodeUrl(const uint8_t* encoded, size_t encodedBytes, size_t limit, std::vector<uint8_t>& output) {
    const Huffman<Model::UrlLengths.size()> h{Model::UrlLengths};
    BitReader reader{encoded, encodedBytes}; output.clear();
    for (;;) {
        unsigned symbol;
        if (!h.Get(reader, symbol)) return false;
        if (symbol == UrlEnd) return !output.empty() && reader.Padding();
        if (symbol < Model::UrlDirect.size()) {
            if (output.size() >= limit) return false;
            output.push_back(Model::UrlDirect[symbol]);
        } else if (symbol == UrlUpper) {
            unsigned value = 0, bit;
            for (unsigned i = 0; i < 5; ++i) { if (!reader.Bit(bit)) return false; value = (value << 1) | bit; }
            if (value >= 26 || output.size() >= limit) return false;
            output.push_back(static_cast<uint8_t>('A' + value));
        } else if (symbol < UrlTokenBase + Model::UrlTokens.size()) {
            auto token = Model::UrlTokens[symbol - UrlTokenBase];
            if (token.size() > limit - output.size()) return false;
            output.insert(output.end(), token.begin(), token.end());
        } else if (symbol == UrlEscape) {
            unsigned byte;
            if (output.size() >= limit || !reader.Bit(byte)) return false;
            for (unsigned i = 1; i < 8; ++i) { unsigned bit; if (!reader.Bit(bit)) return false; byte = (byte << 1) | bit; }
            output.push_back(static_cast<uint8_t>(byte));
        } else if (symbol == UrlCopy) {
            unsigned distance, delta;
            if (!GetGamma(reader, distance) || !GetGamma(reader, delta) || !distance || distance > output.size() || delta > limit - output.size() || delta + 2 > limit - output.size()) return false;
            unsigned length = delta + 2;
            if (length < 3) return false;
            while (length--) output.push_back(output[output.size() - distance]);
        } else return false;
    }
}

struct GhParts { std::string owner, repository, app; };
bool GitHubChar(char c) { return Model::GitHubAlphabet.find(c) != std::string_view::npos; }
bool ParseGitHub(const std::vector<uint8_t>& input, GhParts& parts) {
    std::string text(input.begin(), input.end()); size_t slash = text.find('/'), hash = text.find('#');
    if (!slash || slash == text.npos || text.find('/', slash + 1) != text.npos ||
        (hash != text.npos && (hash <= slash + 1 || hash + 1 == text.size() || text.find('#', hash + 1) != text.npos))) return false;
    parts.owner = text.substr(0, slash);
    parts.repository = text.substr(slash + 1, (hash == text.npos ? text.size() : hash) - slash - 1);
    parts.app = hash == text.npos ? std::string{} : text.substr(hash + 1);
    if (parts.repository.empty()) return false;
    return std::all_of(parts.owner.begin(), parts.owner.end(), GitHubChar) &&
           std::all_of(parts.repository.begin(), parts.repository.end(), GitHubChar) &&
           std::all_of(parts.app.begin(), parts.app.end(), [](char c) { return c != '.' && c != '_' && GitHubChar(c); });
}

std::vector<Action> ParseGhField(std::string_view value, const std::array<std::string_view, 2>& sources, unsigned sourceCount) {
    if (value == sources[0]) return {{Action::Exact, 0, static_cast<unsigned>(value.size())}};
    if (sourceCount > 1 && value == sources[1]) return {{Action::Exact, 1, static_cast<unsigned>(value.size())}};
    size_t n = value.size(); std::vector<unsigned> best(n + 1, std::numeric_limits<unsigned>::max()); std::vector<Action> action(n);
    best[n] = Model::GitHubLengths[GhEnd];
    for (size_t position = n; position-- > 0;) {
        unsigned literalSymbol = static_cast<unsigned>(Model::GitHubAlphabet.find(value[position]));
        best[position] = Model::GitHubLengths[literalSymbol] + best[position + 1]; action[position] = {Action::Literal, literalSymbol, 1};
        for (unsigned sourceId = 0; sourceId < sourceCount; ++sourceId) {
            auto source = sources[sourceId];
            if (source.size() <= n - position && value.substr(position, source.size()) == source) {
                unsigned literal = 0;
                for (char c : source) literal += Model::GitHubLengths[Model::GitHubAlphabet.find(c)];
                unsigned operation = Model::GitHubLengths[GhWholeOwner + sourceId];
                unsigned cost = operation + best[position + source.size()];
                if (operation < literal && (cost < best[position] || (cost == best[position] && source.size() > action[position].length))) {
                    best[position] = cost; action[position] = {Action::Whole, sourceId, static_cast<unsigned>(source.size())};
                }
            }
            for (unsigned offset = 0; offset < source.size(); ++offset) {
                unsigned match = 0;
                while (position + match < n && offset + match < source.size() && value[position + match] == source[offset + match]) ++match;
                unsigned literal = 0;
                for (unsigned length = 1; length <= match; ++length) {
                    literal += Model::GitHubLengths[Model::GitHubAlphabet.find(value[position + length - 1])];
                    if (length < 3) continue;
                    unsigned command = sourceId ? GhCopyRepository : GhCopyOwner;
                    unsigned operation = Model::GitHubLengths[command] + GammaBits(offset + 1) + GammaBits(length);
                    if (operation >= literal) continue;
                    unsigned cost = operation + best[position + length];
                    if (cost < best[position] || (cost == best[position] && length > action[position].length)) {
                        best[position] = cost; action[position] = {Action::Copy, sourceId * 256 + offset, length};
                    }
                }
            }
        }
    }
    std::vector<Action> result;
    for (size_t position = 0; position < n;) { result.push_back(action[position]); position += action[position].length; }
    result.push_back({Action::End, 0, 0}); return result;
}

void PutGhActions(BitWriter& writer, const std::vector<Action>& actions) {
    const Huffman<Model::GitHubLengths.size()> h{Model::GitHubLengths};
    for (auto a : actions) {
        if (a.kind == Action::Literal) h.Put(writer, a.value);
        else if (a.kind == Action::End) h.Put(writer, GhEnd);
        else if (a.kind == Action::Exact) h.Put(writer, a.value ? GhExactRepository : GhExactOwner);
        else if (a.kind == Action::Whole) h.Put(writer, a.value ? GhWholeRepository : GhWholeOwner);
        else { unsigned source = a.value / 256, offset = a.value % 256; h.Put(writer, source ? GhCopyRepository : GhCopyOwner); PutGamma(writer, offset + 1); PutGamma(writer, a.length); }
    }
}

bool EncodeGitHub(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    GhParts p; if (!ParseGitHub(input, p)) return false;
    BitWriter writer; const Huffman<Model::GitHubLengths.size()> h{Model::GitHubLengths};
    for (char c : p.owner) h.Put(writer, static_cast<unsigned>(Model::GitHubAlphabet.find(c)));
    h.Put(writer, GhEnd);
    PutGhActions(writer, ParseGhField(p.repository, {p.owner, {}}, 1));
    writer.Put(p.app.empty() ? 0 : 1, 1);
    if (!p.app.empty()) PutGhActions(writer, ParseGhField(p.app, {p.owner, p.repository}, 2));
    if (writer.bytes.empty() || writer.bytes.size() > MaxEncodedBodyBytes) return false;
    output = std::move(writer.bytes); return true;
}

bool ReadGhField(BitReader& reader, const std::array<std::string_view, 2>& sources, unsigned sourceCount,
                 size_t limit, bool app, std::string& output) {
    const Huffman<Model::GitHubLengths.size()> h{Model::GitHubLengths}; output.clear();
    for (;;) {
        unsigned symbol; if (!h.Get(reader, symbol)) return false;
        if (symbol < GhEnd) {
            char c = Model::GitHubAlphabet[symbol]; if ((app && (c == '.' || c == '_')) || output.size() >= limit) return false; output.push_back(c);
        } else if (symbol == GhEnd) return !output.empty();
        else if (symbol == GhExactOwner || symbol == GhExactRepository) {
            unsigned source = symbol - GhExactOwner;
            if (!output.empty() || source >= sourceCount || sources[source].empty() || sources[source].size() > limit) return false;
            output.assign(sources[source]); return true;
        } else if (symbol == GhWholeOwner || symbol == GhWholeRepository) {
            unsigned source = symbol - GhWholeOwner;
            if (source >= sourceCount || sources[source].empty() || sources[source].size() > limit - output.size()) return false;
            output.append(sources[source]);
        } else if (symbol == GhCopyOwner || symbol == GhCopyRepository) {
            unsigned source = symbol - GhCopyOwner, offset, length;
            if (source >= sourceCount || !GetGamma(reader, offset) || !GetGamma(reader, length) || !offset--) return false;
            if (length < 3 || offset > sources[source].size() || length > sources[source].size() - offset || length > limit - output.size()) return false;
            output.append(sources[source].substr(offset, length));
        } else return false;
    }
}

bool DecodeGitHub(const uint8_t* encoded, size_t encodedBytes, size_t limit, std::vector<uint8_t>& body) {
    BitReader reader{encoded, encodedBytes}; std::string owner, repository, app;
    if (!ReadGhField(reader, std::array<std::string_view, 2>{{{}, {}}}, 0, limit, false, owner) ||
        !ReadGhField(reader, std::array<std::string_view, 2>{{owner, {}}}, 1, limit, false, repository)) return false;
    unsigned present; if (!reader.Bit(present)) return false;
    if (present && !ReadGhField(reader, std::array<std::string_view, 2>{{owner, repository}}, 2, limit, true, app)) return false;
    std::string complete = owner + '/' + repository + (present ? '#' + app : std::string{});
    if (complete.size() > limit || !reader.Padding()) return false;
    body.assign(complete.begin(), complete.end()); return true;
}
} // namespace

bool EncodeOverlayIdentity(const std::wstring& identity, const uint8_t* secret, size_t secretBytes,
                           Validator validator, std::vector<uint8_t>& overlay) {
    overlay.clear();
    if ((secretBytes && !secret) || !Canonical(identity, validator)) return false;
    if (!secretBytes && identity == OneKBIdentity) { overlay.push_back(BuiltInOneKB); return true; }
    uint8_t kind; size_t remove;
    if (identity.rfind(L"gh:", 0) == 0) { kind = GitHubKind; remove = 3; }
    else if (identity.rfind(L"url:https://", 0) == 0) { kind = HttpsKind; remove = 12; }
    else if (identity.rfind(L"url:http://", 0) == 0) { kind = HttpKind; remove = 11; }
    else return false;
    std::vector<uint8_t> input, body;
    std::wstring wireIdentity = identity.substr(remove);
    if (!Utf8(wireIdentity, input)) return false;
    if (kind == GitHubKind ? !EncodeGitHub(input, body) : !EncodeUrl(input, body)) return false;
    overlay = body;
    if (secretBytes) overlay.insert(overlay.end(), secret, secret + secretBytes);
    if (body.size() >= ExtendedLength) overlay.push_back(static_cast<uint8_t>(body.size()));
    overlay.push_back(static_cast<uint8_t>(kind | (body.size() >= ExtendedLength ? ExtendedLength : body.size())));
    return true;
}

bool DecodeOverlayIdentity(const uint8_t* data, size_t dataBytes, size_t secretBytes,
                           size_t maxCanonicalBytes, Validator validator, Decoded& decoded) {
    decoded = Decoded{};
    if (!data || !dataBytes) return false;
    uint8_t typed = data[dataBytes - 1];
    if (typed == BuiltInOneKB) {
        constexpr size_t canonicalBytes = sizeof("gh:1kb-exe/1kb") - 1;
        if (secretBytes || canonicalBytes > maxCanonicalBytes) return false;
        decoded.identity = OneKBIdentity;
        if (!Canonical(decoded.identity, validator)) return false;
        decoded.encodedBodyOffset = dataBytes - 1; decoded.secretOffset = dataBytes - 1; decoded.trailerBytes = 1;
        return true;
    }
    uint8_t kind = typed & KindMask, length = typed & LengthMask;
    if (kind == ReservedKind || !length) return false;
    size_t trailerBytes = 1, encodedBytes = length;
    if (length == ExtendedLength) {
        if (dataBytes < secretBytes + 2) return false;
        encodedBytes = data[dataBytes - 2]; trailerBytes = 2;
        if (encodedBytes < ExtendedLength) return false;
    }
    if (!encodedBytes || encodedBytes > MaxEncodedBodyBytes || dataBytes < encodedBytes + secretBytes + trailerBytes) return false;
    size_t bodyAt = dataBytes - encodedBytes - secretBytes - trailerBytes;
    size_t prefixBytes = kind == GitHubKind ? 3 : kind == HttpsKind ? 12 : 11;
    if (prefixBytes > maxCanonicalBytes) return false;
    std::vector<uint8_t> body;
    bool ok = kind == GitHubKind
        ? DecodeGitHub(data + bodyAt, encodedBytes, maxCanonicalBytes - prefixBytes, body)
        : DecodeUrl(data + bodyAt, encodedBytes, maxCanonicalBytes - prefixBytes, body);
    if (!ok) return false;
    const char* prefix = kind == GitHubKind ? "gh:" : kind == HttpsKind ? "url:https://" : "url:http://";
    std::vector<uint8_t> complete(prefix, prefix + prefixBytes); complete.insert(complete.end(), body.begin(), body.end());
    if (complete.size() > maxCanonicalBytes || !Wide(complete, decoded.identity)) return false;
    if (!Canonical(decoded.identity, validator)) return false;
    decoded.encodedBodyOffset = bodyAt; decoded.encodedBodyBytes = encodedBytes;
    decoded.secretOffset = bodyAt + encodedBytes; decoded.trailerBytes = trailerBytes;
    return true;
}
} // namespace OverlayIdentity
